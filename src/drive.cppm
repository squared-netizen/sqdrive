// SPDX-License-Identifier: MIT
//
// sq.drive — the Virtual Cartridge Drive: the big-file interface for
// squared apps. Mount a 3.0 cartridge, open its entries as files, and read
// and write them by range. sqcart (sq.cart, sq.cart.write) is the only
// thing that writes the format; the Drive owns staging, caching, commit
// policy and recovery ("Virtual Cartridge Drive: design").
//
// Reads go through a cache of decoded chunks. Writes change 1 MiB chunks in
// memory; sync() appends them to a journal in the Drive's own storage and
// flushes it, so they survive a crash; a commit hands them to sqcart as one
// commit, then starts the journal again. At mount, a journal the cartridge
// has not moved past is replayed; one whose commit landed is dropped; one
// the cartridge has moved past otherwise is a conflict (Code::conflict).

module;

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module sq.drive;

import std;
import sq.cart;
import sq.cart.write;

export namespace sq::drive {

using sq::cart::Code;
using sq::cart::Entry;
using sq::cart::Error;
using sq::cart::Result;

/// How a cartridge is mounted.
enum class Access : std::uint8_t {
    read_only,   ///< no lock, no journal; sees the cartridge as it was when mounted
    read_write,  ///< holds the writer lock (§10.2) until unmount; a journal; commits
};

/// When a read-write Drive commits on its own.
enum class Policy : std::uint8_t {
    budget,         ///< past commit_bytes of changes, or commit_after since the first, and at unmount
    explicit_only,  ///< only on commit(); unmount syncs, and the journal waits for the next mount
    on_unmount,     ///< on commit() and at unmount
};

struct MountOptions {
    Access access = Access::read_only;
    /// Decoded chunks kept in memory, in bytes. 0 keeps none.
    std::size_t cache_bytes = std::size_t{64} << 20;
    /// Where journals live, one folder per cartridge. Empty: $XDG_STATE_HOME/sqdrive,
    /// else ~/.local/state/sqdrive. An Android app passes its files directory.
    std::filesystem::path staging;
    Policy commit = Policy::budget;
    std::uint64_t commit_bytes = std::uint64_t{64} << 20;
    std::chrono::milliseconds commit_after{30'000};
    /// Changes held at most, in bytes: past it, a write commits first
    /// (budget) or is refused with Code::limit.
    std::uint64_t staging_bytes = std::uint64_t{1} << 30;
    /// A journal the cartridge has moved past (another writer committed
    /// after a crash): set it aside as journal.conflict-<time> and start
    /// again, rather than refuse the mount with Code::conflict.
    bool discard_journal = false;
    sq::cart::ReadOptions read{};
    /// How commits are written (level, sync, lock); its limits are the reader's.
    sq::cart::WriteOptions write{};
};

/// How a file is opened: mode::read, or mode::write with create and truncate.
namespace mode {
inline constexpr unsigned read = 0, write = 1, create = 2, truncate = 4;
}  // namespace mode

/// What the cache has done since the mount.
struct CacheStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::size_t bytes = 0, units = 0;  ///< held now
};

class Drive;

/// An open file: a path on the Drive. Reads and writes go to whatever is
/// at that path now (after a rename, the old path is gone).
class File {
public:
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// Its size now, staged changes included; 0 if it is gone.
    [[nodiscard]] std::uint64_t size() const;
    /// Its record, as a commit would write it (size and all).
    [[nodiscard]] Result<Entry> stat() const;

    /// Read into `out` from `offset`: as many bytes as fit and the file
    /// holds there (0 at or past its end). Every committed byte is verified.
    Result<std::size_t> read(std::uint64_t offset, std::span<std::byte> out) const;
    /// Up to `n` bytes from `offset`, as a new vector.
    Result<std::vector<std::byte>> read(std::uint64_t offset, std::size_t n) const {
        const std::uint64_t s = size();
        std::vector<std::byte> out(static_cast<std::size_t>(std::min<std::uint64_t>(n, offset < s ? s - offset : 0)));
        auto got = read(offset, std::span(out));
        if (!got) return std::unexpected(got.error());
        out.resize(*got);
        return out;
    }
    /// Write `bytes` at `offset`, growing the file (gaps read as zeros).
    Result<void> write(std::uint64_t offset, std::span<const std::byte> bytes);
    /// Cut it, or grow it with zeros.
    Result<void> resize(std::uint64_t size);
    /// Make every change so far survive a crash (the whole Drive's: one journal).
    Result<void> sync();

private:
    friend class Drive;
    struct State;
    File(std::shared_ptr<State> state, std::string path, std::optional<Entry> entry)
        : state_(std::move(state)), path_(std::move(path)), entry_(std::move(entry)) {}
    std::shared_ptr<State> state_;
    std::string path_;
    std::optional<Entry> entry_;  // a read-only mount's record, kept
};

/// A mounted cartridge. Thread-safe: any number of threads may use one
/// Drive and its Files, and Files outlive the Drive they came from (a read
/// still works after unmount; a write does not).
class Drive {
public:
    [[nodiscard]] static Result<Drive> mount(const std::filesystem::path& cartridge, MountOptions options = {});
    Drive(Drive&&) noexcept = default;
    Drive& operator=(Drive&&) noexcept = default;
    /// Unmounts, as unmount() does, if that was not done.
    ~Drive();

    /// A file, by its path in the cartridge. mode::create makes an empty
    /// one where there is none; mode::truncate empties it.
    [[nodiscard]] Result<File> open(std::string_view path, unsigned how = mode::read) const;
    /// A file's record, as a commit would write it.
    [[nodiscard]] Result<Entry> stat(std::string_view path) const;
    /// Every file whose path starts with `prefix`, sorted by path.
    [[nodiscard]] Result<std::vector<Entry>> list(std::string_view prefix = {}) const;

    Result<void> remove(std::string_view path);
    /// Give a file a new path, replacing any file there. Its content is
    /// not copied: the commit names the same runs (§3.6).
    Result<void> rename(std::string_view from, std::string_view to);
    /// Make every change so far survive a crash.
    Result<void> sync();
    /// Commit every change as one sqcart commit; the current commit when
    /// there is nothing to commit.
    Result<sq::cart::Commit> commit();
    /// Commit or sync as the policy says, release the lock, end writing.
    Result<void> unmount();

    /// The commit the Drive reads, without what is staged.
    [[nodiscard]] sq::cart::Commit head() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    /// Bytes of changes held, synced or not, not yet committed.
    [[nodiscard]] std::uint64_t staged_bytes() const;
    /// The journal of a read-write mount.
    [[nodiscard]] std::filesystem::path journal_path() const;
    [[nodiscard]] CacheStats cache_stats() const;
    /// Drop every cached chunk (a cold cache, for measuring).
    void drop_cache();

private:
    explicit Drive(std::shared_ptr<File::State> state) : state_(std::move(state)) {}
    std::shared_ptr<File::State> state_;
};

}  // namespace sq::drive

namespace sq::drive {

namespace fs = std::filesystem;
using sq::cart::chunk_size;

[[nodiscard]] std::unexpected<Error> fail(Code code, std::string message, std::optional<std::string> entry = std::nullopt) {
    return std::unexpected(Error{code, std::move(message), std::move(entry)});
}

std::string errno_text(std::string_view what, const fs::path& path) { return std::format("{} {}: {}", what, path.string(), std::strerror(errno)); }

/// Where a test can make the process stop dead (SQDRIVE_CRASH=<point>), as
/// a crash would: before_commit (the journal says a commit is coming) and
/// after_commit (it landed; the journal does not know yet).
void crash_point(std::string_view where) {
    const char* const at = std::getenv("SQDRIVE_CRASH");  // each time: a test sets it in a child
    if (at != nullptr && where == at) std::_Exit(86);
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- the cache

/// One decoded unit: a chunk of a chunked entry (§4.5), or the whole of
/// any other (at most 64 MiB). Keyed by the entry's digest and the chunk's
/// index, so an unchanged chunk keeps its place across commits and paths.
struct Key {
    sq::cart::Digest digest{};
    std::uint64_t index = 0;
    friend bool operator==(const Key&, const Key&) = default;
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        std::size_t h = 0;
        for (std::size_t i = 0; i < sizeof(std::size_t); ++i) h = h << 8 | std::to_integer<std::size_t>(k.digest[i]);
        return h ^ (std::hash<std::uint64_t>{}(k.index) * 0x9e3779b97f4a7c15ull);
    }
};

/// A least-recently-used cache of decoded units, bounded in bytes. A unit
/// larger than the whole budget is read but not kept.
class Cache {
public:
    using Unit = std::shared_ptr<const std::vector<std::byte>>;

    explicit Cache(std::size_t budget) : budget_(budget) {}

    Unit find(const Key& k) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(k);
        if (it == map_.end()) {
            ++stats_.misses;
            return nullptr;
        }
        ++stats_.hits;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    void put(const Key& k, Unit unit) {
        std::lock_guard lock(mutex_);
        if (unit->size() > budget_ || map_.contains(k)) return;
        order_.emplace_front(k, std::move(unit));
        map_.emplace(k, order_.begin());
        stats_.bytes += order_.front().second->size();
        while (stats_.bytes > budget_) {
            auto& [key, victim] = order_.back();
            stats_.bytes -= victim->size();
            map_.erase(key);
            order_.pop_back();
            ++stats_.evictions;
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        order_.clear();
        map_.clear();
        stats_.bytes = 0;
    }

    CacheStats stats() const {
        std::lock_guard lock(mutex_);
        CacheStats s = stats_;
        s.units = map_.size();
        return s;
    }

private:
    std::size_t budget_;
    mutable std::mutex mutex_;
    std::list<std::pair<Key, Unit>> order_;  // most recent first
    std::unordered_map<Key, std::list<std::pair<Key, Unit>>::iterator, KeyHash> map_;
    CacheStats stats_;
};

// -------------------------------------------------------------- the journal

/// CRC-32C (Castagnoli), as iSCSI and ext4 use it.
constexpr auto crc_table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0x82f63b78u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}();

std::uint32_t crc32c(std::span<const std::byte> bytes, std::uint32_t crc = 0) noexcept {
    crc = ~crc;
    for (const std::byte b : bytes) crc = crc_table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/// The journal's records. Each is its payload's length (u32), its type
/// (u8), the payload, and a CRC-32C of type and payload (u32). Only what
/// comes before a `sync` record whose CRC passes is replayed.
enum class Rec : std::uint8_t {
    entry = 1,       ///< a path's state: flags (removed, reset), its base's path, size, floor
    chunk = 2,       ///< a path's chunk: index, its bytes now
    rename = 3,      ///< from, to: what is staged at `from` moves to `to`
    sync = 4,        ///< everything before is durable
    committing = 5,  ///< the time (ms) of the commit about to be made
};

constexpr std::string_view journal_magic = "SQDRIVE1";
constexpr std::size_t journal_header = 20;  // magic, base commit sequence (u64), CRC-32C of both (u32)

void put_u(std::vector<std::byte>& out, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
}
void put_s(std::vector<std::byte>& out, std::string_view s) {
    put_u(out, s.size(), 2);
    const auto b = std::as_bytes(std::span(s));
    out.insert(out.end(), b.begin(), b.end());
}

/// Reading a record's payload: every read checked against its end.
struct Cursor {
    std::span<const std::byte> b;
    std::size_t at = 0;
    bool ok = true;
    std::uint64_t u(int n) {
        if (b.size() - at < static_cast<std::size_t>(n)) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = v << 8 | std::to_integer<std::uint64_t>(b[at + static_cast<std::size_t>(i)]);
        at += static_cast<std::size_t>(n);
        return v;
    }
    std::string s() {
        const auto n = static_cast<std::size_t>(u(2));
        if (!ok || b.size() - at < n) {
            ok = false;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(b.data() + at), n);
        at += n;
        return out;
    }
    std::span<const std::byte> rest() {
        auto r = b.subspan(at);
        at = b.size();
        return r;
    }
};

struct Record {
    Rec type;
    std::vector<std::byte> payload;
};

/// What a journal holds: its base, and its records up to its last sync.
struct JournalContents {
    std::uint64_t base = 0;
    std::vector<Record> records;
    std::uint64_t end = journal_header;            ///< just past the last sync
    std::optional<std::int64_t> committing;        ///< the last commit it was about to make
};

/// An append-only journal file: records gather in memory and reach the
/// file, flushed to storage, on flush().
class Journal {
public:
    Journal() = default;
    Journal(Journal&& o) noexcept : fd_(std::exchange(o.fd_, -1)), path_(std::move(o.path_)), end_(o.end_), pending_(std::move(o.pending_)) {}
    Journal& operator=(Journal&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
            path_ = std::move(o.path_);
            end_ = o.end_;
            pending_ = std::move(o.pending_);
        }
        return *this;
    }
    ~Journal() { close(); }

    /// A new, empty journal on `base`: written beside, flushed, renamed over
    /// any old one, the folder flushed; a crash leaves the old or the new.
    static Result<Journal> create(const fs::path& path, std::uint64_t base, bool sync) {
        fs::path temporary = path;
        temporary += ".new";
        std::vector<std::byte> header;
        const auto m = std::as_bytes(std::span(journal_magic));
        header.insert(header.end(), m.begin(), m.end());
        put_u(header, base, 8);
        put_u(header, crc32c(header), 4);
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return fail(Code::io, errno_text("cannot create", temporary));
        const bool written = ::write(fd, header.data(), header.size()) == static_cast<ssize_t>(header.size()) && (!sync || ::fsync(fd) == 0);
        ::close(fd);
        if (!written) return fail(Code::io, errno_text("cannot write", temporary));
        if (::rename(temporary.c_str(), path.c_str()) != 0) return fail(Code::io, errno_text("cannot rename over", path));
        if (sync) {
            if (auto r = flush_folder(path); !r) return std::unexpected(r.error());
        }
        return append_to(path, journal_header);
    }

    /// An existing journal, opened to append after `end` (a torn tail past
    /// its last sync is cut off).
    static Result<Journal> append_to(const fs::path& path, std::uint64_t end) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return fail(Code::io, errno_text("cannot open", path));
        if (::ftruncate(fd, static_cast<off_t>(end)) != 0) {
            ::close(fd);
            return fail(Code::io, errno_text("cannot truncate", path));
        }
        Journal j;
        j.fd_ = fd;
        j.path_ = path;
        j.end_ = end;
        return j;
    }

    static Result<JournalContents> read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail(Code::io, "cannot read " + path.string());
        std::vector<char> text{std::istreambuf_iterator<char>(in), {}};
        const auto b = std::as_bytes(std::span(text));
        JournalContents c;
        if (b.size() < journal_header || std::string_view(text.data(), 8) != journal_magic) {
            return fail(Code::malformed, path.string() + " is not a Drive journal");
        }
        Cursor h{b.subspan(8, 12)};
        c.base = h.u(8);
        if (h.u(4) != crc32c(b.first(16))) return fail(Code::malformed, path.string() + ": its header does not match its CRC");
        std::vector<Record> tentative;
        std::optional<std::int64_t> committing;
        for (std::size_t at = journal_header; b.size() - at >= 9;) {
            Cursor r{b.subspan(at)};
            const auto len = static_cast<std::size_t>(r.u(4));
            if (b.size() - at - 9 < len) break;  // torn
            const auto type_and_payload = b.subspan(at + 4, 1 + len);
            Cursor t{b.subspan(at + 5 + len, 4)};
            if (t.u(4) != crc32c(type_and_payload)) break;  // torn or damaged: what follows is not trusted
            const auto type = static_cast<Rec>(std::to_integer<std::uint8_t>(type_and_payload[0]));
            const auto payload = type_and_payload.subspan(1);
            at += 9 + len;
            if (type == Rec::sync) {
                std::ranges::move(tentative, std::back_inserter(c.records));
                tentative.clear();
                if (committing) c.committing = committing;
                c.end = at;
                continue;
            }
            if (type == Rec::committing) {
                Cursor p{payload};
                committing = static_cast<std::int64_t>(p.u(8));
                continue;
            }
            tentative.push_back({type, std::vector<std::byte>(payload.begin(), payload.end())});
        }
        return c;
    }

    void add(Rec type, std::span<const std::byte> payload) {
        put_u(pending_, payload.size(), 4);
        const std::byte t{static_cast<std::uint8_t>(type)};
        pending_.push_back(t);
        pending_.insert(pending_.end(), payload.begin(), payload.end());
        put_u(pending_, crc32c(payload, crc32c(std::span(&t, 1))), 4);
    }

    /// Append what was added and a sync record, and flush to storage.
    Result<void> flush(bool sync) {
        add(Rec::sync, {});
        std::size_t done = 0;
        while (done < pending_.size()) {
            const ssize_t n = ::pwrite(fd_, pending_.data() + done, pending_.size() - done, static_cast<off_t>(end_ + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                pending_.clear();
                return fail(Code::io, errno_text("cannot write", path_));
            }
            done += static_cast<std::size_t>(n);
        }
        if (sync && ::fsync(fd_) != 0) {
            pending_.clear();
            return fail(Code::io, errno_text("cannot flush", path_));
        }
        end_ += pending_.size();
        pending_.clear();
        return {};
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    static Result<void> flush_folder(const fs::path& file) {
        const int fd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return fail(Code::io, errno_text("cannot open the folder of", file));
        const int r = ::fsync(fd);
        ::close(fd);
        if (r != 0 && errno != EINVAL) return fail(Code::io, errno_text("cannot flush the folder of", file));
        return {};
    }

private:
    void close() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
    fs::path path_;
    std::uint64_t end_ = journal_header;
    std::vector<std::byte> pending_;
};

// -------------------------------------------------------------- the overlay

/// What is staged at one path: nothing there (removed), or a file built on
/// a committed entry (`base`, renamed or not) or on nothing (a new file),
/// `size` bytes long, its changed chunks in `chunks` (each holding all its
/// bytes now: 1 MiB, or less for the last). Bytes of no changed chunk are
/// the base's below `floor` (the lowest size it was cut to), else zeros.
struct Overlay {
    bool removed = false;
    std::optional<Entry> base;
    std::uint64_t size = 0, floor = 0;
    std::map<std::uint64_t, std::vector<std::byte>> chunks;
    // Since the last sync:
    bool dirty = false, reset = false;
    std::set<std::uint64_t> dirty_chunks;
};

/// An entry's metadata as PutOptions, to put it again as it was.
sq::cart::PutOptions options_of(const Entry& e) {
    namespace flag = sq::cart::flag;
    sq::cart::PutOptions o;
    o.title = e.title, o.author = e.author, o.media_type = e.media_type, o.attributes = e.attributes;
    o.created = e.created, o.updated = e.updated;
    o.flags = e.flags & (flag::r | flag::w | flag::x | flag::mappable | flag::dictionary);
    o.store = (e.flags & flag::mappable) ? sq::cart::Store::stored : sq::cart::Store::automatic;
    o.dictionary = e.codec == sq::cart::Codec::zstd_dict ? e.dictionary : 0;
    return o;
}

fs::path default_staging() {
    if (const char* x = std::getenv("XDG_STATE_HOME"); x != nullptr && *x != 0) return fs::path(x) / "sqdrive";
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != 0) return fs::path(h) / ".local" / "state" / "sqdrive";
    return fs::temp_directory_path() / "sqdrive";
}

/// The folder for a cartridge's journal: a hash of its canonical path, and
/// its file name for people.
fs::path staging_folder(const fs::path& staging, const fs::path& cartridge) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(fs::absolute(cartridge), ec);
    if (ec) canonical = fs::absolute(cartridge);
    const auto text = canonical.string();
    const auto d = sq::cart::sha256(std::as_bytes(std::span(text)));
    std::string name;
    for (std::size_t i = 0; i < 8; ++i) name += std::format("{:02x}", std::to_integer<unsigned>(d[i]));
    std::string readable = cartridge.filename().string();
    for (char& c : readable) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
    }
    return staging / (name + "-" + readable);
}

struct File::State {
    State(fs::path p, MountOptions o, sq::cart::Archive a)
        : path(std::move(p)), options(std::move(o)), archive(std::move(a)), cache(options.cache_bytes) {}

    fs::path path;
    MountOptions options;
    mutable std::shared_mutex m;
    sq::cart::Archive archive;
    Cache cache;
    // A read-write mount:
    std::optional<sq::cart::Writer> writer;
    Journal journal;
    std::map<std::string, Overlay, std::less<>> overlays;
    std::vector<std::pair<std::string, std::string>> renames;  // since the last sync
    std::uint64_t staged = 0;                                  // bytes in overlay chunks
    std::optional<std::chrono::steady_clock::time_point> first_change;
    bool writable = false;

    [[nodiscard]] const sq::cart::Limits& limits() const noexcept { return options.read.limits; }

    // ------------------------------------------------------------ reading

    /// Unit `index` of `e`: from the cache, or read, verified and kept.
    Result<Cache::Unit> unit(const Entry& e, std::uint64_t index) {
        const Key k{e.digest, index};
        if (auto u = cache.find(k)) return u;
        const bool chunked = e.codec == sq::cart::Codec::chunked;
        const std::uint64_t lo = chunked ? index * chunk_size : 0;
        const std::uint64_t n = chunked ? std::min<std::uint64_t>(chunk_size, e.size - lo) : e.size;
        auto bytes = archive.read_range(e, lo, n);
        if (!bytes) return std::unexpected(bytes.error());
        auto u = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        cache.put(k, u);
        return u;
    }

    /// Bytes of a committed entry into `out` from `offset`.
    Result<std::size_t> read_entry(const Entry& e, std::uint64_t offset, std::span<std::byte> out) {
        if (offset >= e.size || out.empty()) return std::size_t{0};
        const std::uint64_t end = std::min<std::uint64_t>(e.size, offset + out.size());
        const bool chunked = e.codec == sq::cart::Codec::chunked;
        const std::uint64_t unit_size = chunked ? chunk_size : e.size;
        std::size_t done = 0;
        for (std::uint64_t at = offset; at < end;) {
            const std::uint64_t index = chunked ? at / unit_size : 0;
            auto u = unit(e, index);
            if (!u) return std::unexpected(u.error());
            const std::uint64_t from = at - index * unit_size;
            const std::uint64_t take = std::min<std::uint64_t>((*u)->size() - from, end - at);
            std::copy_n((*u)->begin() + static_cast<std::ptrdiff_t>(from), static_cast<std::ptrdiff_t>(take), out.begin() + static_cast<std::ptrdiff_t>(done));
            done += static_cast<std::size_t>(take);
            at += take;
        }
        return done;
    }

    /// `out` from `at`, as an overlay's base gives it: the base's bytes
    /// below its floor, zeros above (changed chunks aside).
    Result<void> from_base(const Overlay& ov, std::uint64_t at, std::span<std::byte> out) {
        std::ranges::fill(out, std::byte{0});
        if (!ov.base) return {};
        const std::uint64_t base_end = std::min(ov.floor, ov.base->size);
        if (at >= base_end) return {};
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), base_end - at));
        auto got = read_entry(*ov.base, at, out.first(n));
        if (!got) return std::unexpected(got.error());
        return {};
    }

    Result<std::size_t> read_overlay(const Overlay& ov, std::uint64_t offset, std::span<std::byte> out) {
        if (offset >= ov.size || out.empty()) return std::size_t{0};
        const std::uint64_t end = std::min<std::uint64_t>(ov.size, offset + out.size());
        std::size_t done = 0;
        for (std::uint64_t at = offset; at < end;) {
            const std::uint64_t i = at / chunk_size, lo = i * chunk_size;
            const std::uint64_t take = std::min(lo + chunk_size, end) - at;
            auto piece = out.subspan(done, static_cast<std::size_t>(take));
            if (auto c = ov.chunks.find(i); c != ov.chunks.end()) {
                std::copy_n(c->second.begin() + static_cast<std::ptrdiff_t>(at - lo), static_cast<std::ptrdiff_t>(take), piece.begin());
            } else if (auto r = from_base(ov, at, piece); !r) {
                return std::unexpected(r.error());
            }
            done += static_cast<std::size_t>(take);
            at += take;
        }
        return done;
    }

    /// What a path holds now: its overlay, or its committed entry.
    struct Found {
        const Overlay* overlay = nullptr;
        std::optional<Entry> entry;
    };
    Result<Found> find(std::string_view path) const {
        if (auto it = overlays.find(path); it != overlays.end()) {
            if (it->second.removed) return fail(Code::not_found, "no such file", std::string(path));
            return Found{&it->second, std::nullopt};
        }
        auto e = archive.find(path);
        if (!e) return std::unexpected(e.error());
        return Found{nullptr, std::move(*e)};
    }

    /// The record a commit would write for a path now.
    Result<Entry> stat(std::string_view path) const {
        auto f = find(path);
        if (!f) return std::unexpected(f.error());
        if (!f->overlay) return std::move(*f->entry);
        return as_entry(path, *f->overlay);
    }
    static Entry as_entry(std::string_view path, const Overlay& ov) {
        Entry e;
        if (ov.base) e = *ov.base;
        else e.flags = sq::cart::flag::r | sq::cart::flag::w;
        e.path = std::string(path);
        e.size = ov.size;
        return e;
    }

    // ------------------------------------------------------------ writing

    Result<void> writable_now() const {
        if (!writable) return fail(Code::unsupported, writer ? "the Drive is unmounted" : "the Drive is mounted read-only");
        return {};
    }

    /// The overlay a change to `path` goes to, made from its committed
    /// entry if there is none. A committed entry without `w` (§4.2) is not
    /// changed.
    Result<Overlay*> editable(std::string_view path) {
        if (auto it = overlays.find(path); it != overlays.end()) {
            if (it->second.removed) return fail(Code::not_found, "no such file", std::string(path));
            return &it->second;
        }
        auto e = archive.find(path);
        if (!e) return std::unexpected(e.error());
        if (!(e->flags & sq::cart::flag::w)) return fail(Code::unsupported, "not writable: its entry has no w flag (§4.2)", std::string(path));
        Overlay ov;
        ov.size = ov.floor = e->size;
        ov.base = std::move(*e);
        return &overlays.emplace(std::string(path), std::move(ov)).first->second;
    }

    void note_change() {
        if (!first_change) first_change = std::chrono::steady_clock::now();
    }

    /// Chunk `i` of an overlay, its bytes made from the base if it has
    /// none yet.
    Result<std::vector<std::byte>*> chunk(Overlay& ov, std::uint64_t i) {
        if (auto c = ov.chunks.find(i); c != ov.chunks.end()) return &c->second;
        const std::uint64_t lo = i * chunk_size;
        std::vector<std::byte> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, ov.size - lo)));
        if (auto r = from_base(ov, lo, bytes); !r) return std::unexpected(r.error());
        staged += bytes.size();
        return &ov.chunks.emplace(i, std::move(bytes)).first->second;
    }

    Result<void> resize(Overlay& ov, std::uint64_t n) {
        if (n > limits().max_entry_size) return fail(Code::limit, "past the entry size limit");
        if (n > ov.size) {
            if (ov.size > 0) {
                const std::uint64_t last = (ov.size - 1) / chunk_size;
                if (auto c = ov.chunks.find(last); c != ov.chunks.end()) {
                    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, n - last * chunk_size));
                    staged += want - c->second.size();
                    c->second.resize(want);
                    ov.dirty_chunks.insert(last);
                }
            }
            ov.size = n;
        } else if (n < ov.size) {
            if (n % chunk_size != 0) {
                const std::uint64_t k = n / chunk_size;
                auto c = chunk(ov, k);
                if (!c) return std::unexpected(c.error());
                staged -= (*c)->size() - (n - k * chunk_size);
                (*c)->resize(static_cast<std::size_t>(n - k * chunk_size));
                ov.dirty_chunks.insert(k);
            }
            for (auto it = ov.chunks.lower_bound((n + chunk_size - 1) / chunk_size); it != ov.chunks.end();) {
                staged -= it->second.size();
                ov.dirty_chunks.erase(it->first);
                it = ov.chunks.erase(it);
            }
            ov.size = n;
            ov.floor = std::min(ov.floor, n);
        }
        ov.dirty = true;
        note_change();
        return {};
    }

    Result<void> write(Overlay& ov, std::uint64_t offset, std::span<const std::byte> bytes) {
        if (bytes.empty()) return {};
        if (offset > limits().max_entry_size || bytes.size() > limits().max_entry_size - offset) return fail(Code::limit, "past the entry size limit");
        const std::uint64_t end = offset + bytes.size();
        if (end > ov.size) {
            if (auto r = resize(ov, end); !r) return r;
        }
        for (std::uint64_t at = offset; at < end;) {
            const std::uint64_t i = at / chunk_size, lo = i * chunk_size;
            const std::uint64_t take = std::min(lo + chunk_size, end) - at;
            auto c = chunk(ov, i);
            if (!c) return std::unexpected(c.error());
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at - offset), static_cast<std::ptrdiff_t>(take),
                        (*c)->begin() + static_cast<std::ptrdiff_t>(at - lo));
            ov.dirty_chunks.insert(i);
            at += take;
        }
        ov.dirty = true;
        note_change();
        return {};
    }

    /// Before a change: room for it, committing first where the policy may.
    Result<void> make_room() {
        if (staged < options.staging_bytes) return {};
        if (options.commit == Policy::budget) {
            auto c = commit();
            if (!c) return std::unexpected(c.error());
            return {};
        }
        return fail(Code::limit, std::format("{} bytes staged, the most allowed: commit first", staged));
    }

    /// After a change: commit if the budget says so.
    Result<void> after_change() {
        if (options.commit != Policy::budget) return {};
        const bool full = staged >= options.commit_bytes;
        const bool old = first_change && std::chrono::steady_clock::now() - *first_change >= options.commit_after;
        if (!full && !old) return {};
        auto c = commit();
        if (!c) return std::unexpected(c.error());
        return {};
    }

    // ------------------------------------------------- the journal, commits

    [[nodiscard]] bool anything_staged() const { return !overlays.empty(); }

    Result<void> sync() {
        if (!writable) return {};
        const bool dirty = !renames.empty() || std::ranges::any_of(overlays, [](const auto& o) { return o.second.dirty; });
        if (!dirty) return {};
        std::vector<std::byte> p;
        for (const auto& [from, to] : renames) {
            p.clear();
            put_s(p, from);
            put_s(p, to);
            journal.add(Rec::rename, p);
        }
        for (const auto& [path, ov] : overlays) {
            if (!ov.dirty) continue;
            p.clear();
            put_s(p, path);
            put_u(p, (ov.removed ? 1u : 0u) | (ov.reset ? 4u : 0u) | (ov.base ? 2u : 0u), 1);
            put_s(p, ov.base ? std::string_view(ov.base->path) : std::string_view{});
            put_u(p, ov.size, 8);
            put_u(p, ov.floor, 8);
            journal.add(Rec::entry, p);
            for (const std::uint64_t i : ov.dirty_chunks) {
                auto c = ov.chunks.find(i);
                if (c == ov.chunks.end()) continue;
                p.clear();
                put_s(p, path);
                put_u(p, i, 8);
                p.insert(p.end(), c->second.begin(), c->second.end());
                journal.add(Rec::chunk, p);
            }
        }
        if (auto r = journal.flush(options.write.sync); !r) return r;
        renames.clear();
        for (auto& [path, ov] : overlays) {
            ov.dirty = ov.reset = false;
            ov.dirty_chunks.clear();
        }
        return {};
    }

    /// Apply a journal's records to the overlays, as replay after a crash.
    Result<void> replay(const std::vector<Record>& records) {
        const auto broken = [&](std::string_view why) { return fail(Code::conflict, std::format("{}: {}", journal.path().string(), why)); };
        for (const Record& r : records) {
            Cursor c{r.payload};
            if (r.type == Rec::rename) {
                const std::string from = c.s(), to = c.s();
                if (!c.ok) return broken("a rename record that does not parse");
                auto ov = editable_replay(from);
                if (!ov) return broken(std::format("a rename of {}, which is not there", from));
                Overlay moved = std::move(**ov);
                overlays.insert_or_assign(to, std::move(moved));
                Overlay gone;
                gone.removed = true;
                overlays.insert_or_assign(from, std::move(gone));
            } else if (r.type == Rec::entry) {
                const std::string path = c.s();
                const auto flags = c.u(1);
                const std::string base = c.s();
                const std::uint64_t size = c.u(8), floor = c.u(8);
                if (!c.ok) return broken("an entry record that does not parse");
                Overlay& ov = overlays[path];
                if ((flags & 4) || (flags & 1)) ov.chunks.clear();
                ov.removed = (flags & 1) != 0;
                ov.base.reset();
                if (flags & 2) {
                    auto e = archive.find(base);
                    if (!e) return broken(std::format("{} builds on {}, which the cartridge does not hold", path, base));
                    ov.base = std::move(*e);
                }
                ov.size = size;
                ov.floor = floor;
                for (auto it = ov.chunks.lower_bound((size + chunk_size - 1) / chunk_size); it != ov.chunks.end();) it = ov.chunks.erase(it);
            } else if (r.type == Rec::chunk) {
                const std::string path = c.s();
                const std::uint64_t i = c.u(8);
                const auto bytes = c.rest();
                auto it = overlays.find(path);
                if (!c.ok || it == overlays.end() || it->second.removed) return broken("a chunk record for no file");
                it->second.chunks.insert_or_assign(i, std::vector<std::byte>(bytes.begin(), bytes.end()));
            }
        }
        staged = 0;
        for (const auto& [path, ov] : overlays) {
            for (const auto& [i, bytes] : ov.chunks) staged += bytes.size();
        }
        if (!overlays.empty()) note_change();
        return {};
    }
    Result<Overlay*> editable_replay(std::string_view path) {
        if (auto it = overlays.find(path); it != overlays.end()) {
            if (it->second.removed) return fail(Code::not_found, "removed");
            return &it->second;
        }
        auto e = archive.find(path);
        if (!e) return std::unexpected(e.error());
        Overlay ov;
        ov.size = ov.floor = e->size;
        ov.base = std::move(*e);
        return &overlays.emplace(std::string(path), std::move(ov)).first->second;
    }

    /// Hand the overlays to the writer as one commit's changes.
    Result<std::vector<fs::path>> stage(sq::cart::Writer& w) {
        std::vector<fs::path> temporaries;
        // Renames, through temporary names, so that any permutation (a swap) works.
        std::vector<std::pair<std::string, std::string>> moves;
        std::set<std::string, std::less<>> sources;
        for (const auto& [path, ov] : overlays) {
            if (!ov.removed && ov.base && ov.base->path != path) {
                moves.emplace_back(ov.base->path, path);
                sources.insert(ov.base->path);
            }
        }
        std::vector<std::string> parked;
        for (std::size_t n = 0; const auto& [from, to] : moves) {
            std::string tmp;
            do tmp = std::format("SQ-INF/drive/rename-{}", n++);
            while (w.size(tmp).has_value());
            if (auto r = w.rename(from, tmp); !r) return std::unexpected(r.error());
            parked.push_back(std::move(tmp));
        }
        for (std::size_t k = 0; k < moves.size(); ++k) {
            if (auto r = w.rename(parked[k], moves[k].second); !r) return std::unexpected(r.error());
        }
        // Removals of what was committed and is not moved.
        for (const auto& [path, ov] : overlays) {
            if (ov.removed && !sources.contains(path) && archive.find(path).has_value()) {
                if (auto r = w.remove(path); !r) return std::unexpected(r.error());
            }
        }
        // Contents.
        for (auto& [path, ov] : overlays) {
            if (ov.removed) continue;
            const bool cut = ov.base && ov.floor < ov.base->size;
            const bool changed = !ov.base || cut || ov.size != ov.base->size || !ov.chunks.empty();
            if (!changed) continue;
            if (ov.base && ov.base->codec == sq::cart::Codec::chunked) {
                // Only the chunks changed are written; the rest are named again.
                if (cut) {
                    if (auto r = w.resize(path, ov.floor); !r) return std::unexpected(r.error());
                }
                if (ov.size != (cut ? ov.floor : ov.base->size)) {
                    if (auto r = w.resize(path, ov.size); !r) return std::unexpected(r.error());
                }
                for (const auto& [i, bytes] : ov.chunks) {
                    if (auto r = w.put_range(path, i * chunk_size, bytes); !r) return std::unexpected(r.error());
                }
                continue;
            }
            // Anything else is put whole: in memory up to 64 MiB, else from a file, chunked.
            auto options = ov.base ? options_of(*ov.base) : sq::cart::PutOptions{.flags = sq::cart::flag::r | sq::cart::flag::w};
            if (ov.size <= limits().max_unchunked_size) {
                std::vector<std::byte> content(static_cast<std::size_t>(ov.size));
                auto got = read_overlay(ov, 0, content);
                if (!got) return std::unexpected(got.error());
                if (auto r = w.put(path, content, options); !r) return std::unexpected(r.error());
                continue;
            }
            const fs::path tmp = journal.path().parent_path() / std::format("commit-{}.tmp", temporaries.size());
            temporaries.push_back(tmp);
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            std::vector<std::byte> buf(static_cast<std::size_t>(chunk_size));
            for (std::uint64_t at = 0; at < ov.size; at += chunk_size) {
                auto got = read_overlay(ov, at, buf);
                if (!got) return std::unexpected(got.error());
                out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(*got));
            }
            out.close();
            if (!out) return fail(Code::io, "cannot write " + tmp.string(), path);
            options.store = sq::cart::Store::chunked;
            if (auto r = w.put_file(path, tmp, options); !r) return std::unexpected(r.error());
        }
        return temporaries;
    }

    Result<sq::cart::Commit> commit() {
        if (!writable) {
            if (auto r = writable_now(); !r) return std::unexpected(r.error());
        }
        if (!anything_staged()) return archive.commit();
        if (auto r = sync(); !r) return std::unexpected(r.error());
        // Write down the commit's time first: after a crash, a commit with
        // this time on top of the base is this one.
        const std::int64_t time = std::max(now_ms(), archive.commit().time + 1);
        std::vector<std::byte> p;
        put_u(p, static_cast<std::uint64_t>(time), 8);
        journal.add(Rec::committing, p);
        if (auto r = journal.flush(options.write.sync); !r) return std::unexpected(r.error());
        crash_point("before_commit");
        auto temporaries = stage(*writer);
        const auto clean = [&] {
            if (temporaries) {
                std::error_code ec;
                for (const auto& t : *temporaries) fs::remove(t, ec);
            }
        };
        if (!temporaries) {
            writer->discard();
            return std::unexpected(temporaries.error());
        }
        writer->set_commit_time(time);
        auto c = writer->commit();
        clean();
        if (!c) {
            writer->discard();
            return std::unexpected(c.error());
        }
        crash_point("after_commit");
        auto fresh = Journal::create(journal.path(), c->sequence, options.write.sync);
        if (!fresh) return std::unexpected(fresh.error());
        journal = std::move(*fresh);
        auto a = sq::cart::Archive::open(path, options.read);
        if (!a) return std::unexpected(a.error());
        archive = std::move(*a);
        overlays.clear();
        renames.clear();
        staged = 0;
        first_change.reset();
        return *c;
    }

    Result<void> unmount() {
        if (!writable) return {};
        Result<void> r;
        if (options.commit == Policy::explicit_only) {
            r = sync();
        } else if (auto c = commit(); !c) {
            r = std::unexpected(c.error());
        }
        if (!r) {
            static_cast<void>(sync());  // keep what can be kept for the next mount
        }
        writable = false;
        writer.reset();
        journal = Journal{};
        return r;
    }
};

// ------------------------------------------------------------------ File

std::uint64_t File::size() const {
    if (entry_) return entry_->size;
    std::shared_lock lock(state_->m);
    auto e = state_->stat(path_);
    return e ? e->size : 0;
}

Result<Entry> File::stat() const {
    if (entry_) return *entry_;
    std::shared_lock lock(state_->m);
    return state_->stat(path_);
}

Result<std::size_t> File::read(std::uint64_t offset, std::span<std::byte> out) const {
    if (entry_) return state_->read_entry(*entry_, offset, out);
    std::shared_lock lock(state_->m);
    auto f = state_->find(path_);
    if (!f) return std::unexpected(f.error());
    if (f->overlay) return state_->read_overlay(*f->overlay, offset, out);
    return state_->read_entry(*f->entry, offset, out);
}

Result<void> File::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    std::unique_lock lock(state_->m);
    if (auto r = state_->writable_now(); !r) return r;
    if (auto r = state_->make_room(); !r) return r;
    auto ov = state_->editable(path_);
    if (!ov) return std::unexpected(ov.error());
    if (auto r = state_->write(**ov, offset, bytes); !r) return std::unexpected(Error{r.error().code, r.error().message, path_});
    return state_->after_change();
}

Result<void> File::resize(std::uint64_t size) {
    std::unique_lock lock(state_->m);
    if (auto r = state_->writable_now(); !r) return r;
    if (auto r = state_->make_room(); !r) return r;
    auto ov = state_->editable(path_);
    if (!ov) return std::unexpected(ov.error());
    if (auto r = state_->resize(**ov, size); !r) return std::unexpected(Error{r.error().code, r.error().message, path_});
    return state_->after_change();
}

Result<void> File::sync() {
    std::unique_lock lock(state_->m);
    return state_->sync();
}

// ------------------------------------------------------------------ Drive

Result<Drive> Drive::mount(const fs::path& cartridge, MountOptions options) {
    options.write.limits = options.read.limits;
    if (options.access == Access::read_only) {
        auto archive = sq::cart::Archive::open(cartridge, options.read);
        if (!archive) return std::unexpected(archive.error());
        return Drive(std::make_shared<File::State>(cartridge, std::move(options), std::move(*archive)));
    }
    // The writer's lock first: no one else changes the cartridge from here.
    auto writer = sq::cart::Writer::open(cartridge, options.write);
    if (!writer) return std::unexpected(writer.error());
    auto archive = sq::cart::Archive::open(cartridge, options.read);
    if (!archive) return std::unexpected(archive.error());
    const fs::path folder = staging_folder(options.staging.empty() ? default_staging() : options.staging, cartridge);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return fail(Code::io, std::format("cannot create {}: {}", folder.string(), ec.message()));
    const fs::path journal = folder / "journal";
    const std::uint64_t head = archive->commit().sequence;
    const std::int64_t head_time = archive->commit().time;
    const bool sync = options.write.sync;
    auto state = std::make_shared<File::State>(cartridge, std::move(options), std::move(*archive));
    state->writer = std::move(*writer);
    if (!fs::exists(journal, ec)) {
        auto j = Journal::create(journal, head, sync);
        if (!j) return std::unexpected(j.error());
        state->journal = std::move(*j);
    } else {
        auto contents = Journal::read(journal);
        if (!contents) return std::unexpected(contents.error());
        if (contents->base == head) {
            // Not committed: replay what was synced, drop a torn tail.
            auto j = Journal::append_to(journal, contents->end);
            if (!j) return std::unexpected(j.error());
            state->journal = std::move(*j);
            if (auto r = state->replay(contents->records); !r) return std::unexpected(r.error());
        } else if (contents->committing && head == contents->base + 1 && head_time == *contents->committing) {
            // Its commit landed; only the journal did not hear.
            auto j = Journal::create(journal, head, sync);
            if (!j) return std::unexpected(j.error());
            state->journal = std::move(*j);
        } else if (state->options.discard_journal) {
            fs::path aside = journal;
            aside += std::format(".conflict-{}", now_ms());
            fs::rename(journal, aside, ec);
            if (ec) return fail(Code::io, std::format("cannot set {} aside: {}", journal.string(), ec.message()));
            auto j = Journal::create(journal, head, sync);
            if (!j) return std::unexpected(j.error());
            state->journal = std::move(*j);
        } else {
            return fail(Code::conflict, std::format("{} holds changes made on commit {}, and the cartridge is at commit {}: "
                                                    "another writer committed since (mount with discard_journal to set them aside)",
                                                    journal.string(), contents->base, head));
        }
    }
    state->writable = true;
    return Drive(std::move(state));
}

Drive::~Drive() {
    if (state_) {
        std::unique_lock lock(state_->m);
        static_cast<void>(state_->unmount());
    }
}

Result<File> Drive::open(std::string_view path, unsigned how) const {
    if (!state_->writer && !state_->writable) {
        if (how != mode::read) return fail(Code::unsupported, "the Drive is mounted read-only", std::string(path));
        auto e = state_->archive.find(path);
        if (!e) return std::unexpected(e.error());
        return File(state_, std::string(path), std::move(*e));
    }
    std::unique_lock lock(state_->m);
    if (how != mode::read) {
        if (auto r = state_->writable_now(); !r) return std::unexpected(r.error());
    }
    auto found = state_->find(path);
    if (!found) {
        if (found.error().code != Code::not_found || !(how & mode::create)) return std::unexpected(found.error());
        if (auto problem = sq::cart::path_problem(path, state_->limits().max_path_bytes)) return fail(Code::path, *problem, std::string(path));
        if (path == "SQ-INF/manifest.json" || path.starts_with("SQ-INF/dictionaries/")) return fail(Code::path, "the Drive does not write the manifest or dictionaries", std::string(path));
        Overlay ov;
        ov.reset = ov.dirty = true;
        state_->overlays.insert_or_assign(std::string(path), std::move(ov));
        state_->note_change();
    } else if (how & mode::truncate) {
        auto ov = state_->editable(path);
        if (!ov) return std::unexpected(ov.error());
        if (auto r = state_->resize(**ov, 0); !r) return std::unexpected(r.error());
    } else if (how & mode::write) {
        if (auto ov = state_->editable(path); !ov) return std::unexpected(ov.error());  // refuses one without w
    }
    return File(state_, std::string(path), std::nullopt);
}

Result<Entry> Drive::stat(std::string_view path) const {
    std::shared_lock lock(state_->m);
    return state_->stat(path);
}

Result<std::vector<Entry>> Drive::list(std::string_view prefix) const {
    std::shared_lock lock(state_->m);
    auto live = state_->archive.entries();
    if (!live) return std::unexpected(live.error());
    std::map<std::string, Entry, std::less<>> all;
    const auto first = std::ranges::lower_bound(*live, prefix, {}, [](const Entry& e) -> std::string_view { return e.path; });
    for (auto it = first; it != live->end() && it->path.starts_with(prefix); ++it) all.emplace(it->path, *it);
    for (const auto& [path, ov] : state_->overlays) {
        if (!path.starts_with(prefix)) continue;
        if (ov.removed) all.erase(path);
        else all.insert_or_assign(path, File::State::as_entry(path, ov));
    }
    std::vector<Entry> out;
    for (auto& [path, e] : all) out.push_back(std::move(e));
    return out;
}

Result<void> Drive::remove(std::string_view path) {
    std::unique_lock lock(state_->m);
    if (auto r = state_->writable_now(); !r) return r;
    auto ov = state_->editable(path);
    if (!ov) return std::unexpected(ov.error());
    for (const auto& [i, bytes] : (*ov)->chunks) state_->staged -= bytes.size();
    Overlay gone;
    gone.removed = gone.reset = gone.dirty = true;
    **ov = std::move(gone);
    state_->note_change();
    return state_->after_change();
}

Result<void> Drive::rename(std::string_view from, std::string_view to) {
    std::unique_lock lock(state_->m);
    if (auto r = state_->writable_now(); !r) return r;
    if (from == to) return state_->find(from).has_value() ? Result<void>{} : fail(Code::not_found, "no such file", std::string(from));
    if (auto problem = sq::cart::path_problem(to, state_->limits().max_path_bytes)) return fail(Code::path, *problem, std::string(to));
    for (std::string_view p : {from, to}) {
        if (p == "SQ-INF/manifest.json" || p.starts_with("SQ-INF/")) return fail(Code::path, "the Drive does not move files in SQ-INF/", std::string(p));
    }
    auto source = state_->editable(from);
    if (!source) return std::unexpected(source.error());
    if (auto target = state_->find(to); target && !target->overlay && !(target->entry->flags & sq::cart::flag::w)) {
        return fail(Code::unsupported, "not writable: its entry has no w flag (§4.2)", std::string(to));
    }
    Overlay moved = std::move(**source);
    moved.dirty = true;
    if (auto old = state_->overlays.find(to); old != state_->overlays.end()) {
        for (const auto& [i, bytes] : old->second.chunks) state_->staged -= bytes.size();
    }
    Overlay gone;
    gone.removed = gone.reset = gone.dirty = true;
    state_->overlays.insert_or_assign(std::string(from), std::move(gone));
    state_->overlays.insert_or_assign(std::string(to), std::move(moved));
    state_->renames.emplace_back(from, to);
    state_->note_change();
    return state_->after_change();
}

Result<void> Drive::sync() {
    std::unique_lock lock(state_->m);
    return state_->sync();
}

Result<sq::cart::Commit> Drive::commit() {
    std::unique_lock lock(state_->m);
    return state_->commit();
}

Result<void> Drive::unmount() {
    std::unique_lock lock(state_->m);
    return state_->unmount();
}

sq::cart::Commit Drive::head() const {
    std::shared_lock lock(state_->m);
    return state_->archive.commit();
}
const fs::path& Drive::path() const noexcept { return state_->path; }
std::uint64_t Drive::staged_bytes() const {
    std::shared_lock lock(state_->m);
    return state_->staged;
}
fs::path Drive::journal_path() const {
    std::shared_lock lock(state_->m);
    return state_->journal.path();
}
CacheStats Drive::cache_stats() const { return state_->cache.stats(); }
void Drive::drop_cache() { state_->cache.clear(); }

}  // namespace sq::drive
