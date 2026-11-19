// SPDX-License-Identifier: MIT
//
// sq.drive — the Virtual Cartridge Drive: the big-file interface for
// squared apps. Mount a 3.0 cartridge, open its entries as files, read
// them by range. A cache of decoded chunks, bounded in bytes, keeps what
// was read recently; a read touches only the chunks its range covers.
//
// sqcart (sq.cart, sq.cart.write) is the only thing that writes the
// format; the Drive uses it and owns staging, caching, commit policy and
// recovery (the design: "Virtual Cartridge Drive: design").
//
// This is phase 2: read-only mounts. Writing, the journal and commits are
// phase 3.

export module sq.drive;

import std;
import sq.cart;

export namespace sq::drive {

using sq::cart::Code;
using sq::cart::Entry;
using sq::cart::Error;
using sq::cart::Result;

/// How a cartridge is mounted.
enum class Access : std::uint8_t {
    read_only,   ///< no lock, no journal; sees the cartridge as it was when mounted
    read_write,  ///< (phase 3) the writer lock, the journal, commits
};

struct MountOptions {
    Access access = Access::read_only;
    /// Decoded chunks kept in memory, in bytes. 0 keeps none.
    std::size_t cache_bytes = std::size_t{64} << 20;
    /// What the reader accepts (§3.8 of the specification).
    sq::cart::ReadOptions read{};
};

/// What the cache has done since the mount.
struct CacheStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::size_t bytes = 0, units = 0;  ///< held now
};

class Drive;

/// An open file: an entry of the mounted cartridge.
class File {
public:
    [[nodiscard]] const Entry& entry() const noexcept { return entry_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return entry_.size; }

    /// Read into `out` from `offset`: as many bytes as fit and the file
    /// holds there (0 at or past its end). Every byte is verified (§9.1).
    Result<std::size_t> read(std::uint64_t offset, std::span<std::byte> out) const;

    /// Up to `n` bytes from `offset`, as a new vector.
    Result<std::vector<std::byte>> read(std::uint64_t offset, std::size_t n) const {
        std::vector<std::byte> out(static_cast<std::size_t>(std::min<std::uint64_t>(n, offset < size() ? size() - offset : 0)));
        auto got = read(offset, std::span(out));
        if (!got) return std::unexpected(got.error());
        out.resize(*got);
        return out;
    }

private:
    friend class Drive;
    struct State;
    File(std::shared_ptr<State> state, Entry entry) : state_(std::move(state)), entry_(std::move(entry)) {}
    std::shared_ptr<State> state_;
    Entry entry_;
};

/// A mounted cartridge. Thread-safe: any number of threads may open and
/// read through one Drive, and Files outlive the Drive they came from.
class Drive {
public:
    [[nodiscard]] static Result<Drive> mount(const std::filesystem::path& cartridge, MountOptions options = {});

    /// An entry, by its path in the cartridge.
    [[nodiscard]] Result<File> open(std::string_view path) const;
    /// An entry's record, without opening it.
    [[nodiscard]] Result<Entry> stat(std::string_view path) const;
    /// Every live entry whose path starts with `prefix`, sorted by path.
    [[nodiscard]] Result<std::vector<Entry>> list(std::string_view prefix = {}) const;

    [[nodiscard]] const sq::cart::Commit& commit() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] CacheStats cache_stats() const;
    /// Drop every cached chunk (a cold cache, for measuring).
    void drop_cache();

private:
    explicit Drive(std::shared_ptr<File::State> state) : state_(std::move(state)) {}
    std::shared_ptr<File::State> state_;
};

}  // namespace sq::drive

namespace sq::drive {

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

struct File::State {
    State(std::filesystem::path p, sq::cart::Archive a, std::size_t cache_bytes)
        : path(std::move(p)), archive(std::move(a)), cache(cache_bytes) {}

    std::filesystem::path path;
    sq::cart::Archive archive;
    Cache cache;

    /// Unit `index` of `e`: from the cache, or read, verified and kept.
    Result<Cache::Unit> unit(const Entry& e, std::uint64_t index) {
        const Key k{e.digest, index};
        if (auto u = cache.find(k)) return u;
        const bool chunked = e.codec == sq::cart::Codec::chunked;
        const std::uint64_t lo = chunked ? index * sq::cart::chunk_size : 0;
        const std::uint64_t n = chunked ? std::min<std::uint64_t>(sq::cart::chunk_size, e.size - lo) : e.size;
        auto bytes = archive.read_range(e, lo, n);
        if (!bytes) return std::unexpected(bytes.error());
        auto u = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        cache.put(k, u);
        return u;
    }
};

Result<std::size_t> File::read(std::uint64_t offset, std::span<std::byte> out) const {
    if (offset >= entry_.size || out.empty()) return std::size_t{0};
    const std::uint64_t end = std::min<std::uint64_t>(entry_.size, offset + out.size());
    const bool chunked = entry_.codec == sq::cart::Codec::chunked;
    const std::uint64_t unit_size = chunked ? sq::cart::chunk_size : entry_.size;
    std::size_t done = 0;
    for (std::uint64_t at = offset; at < end;) {
        const std::uint64_t index = chunked ? at / unit_size : 0;
        auto u = state_->unit(entry_, index);
        if (!u) return std::unexpected(u.error());
        const std::uint64_t from = at - index * unit_size;
        const std::uint64_t take = std::min<std::uint64_t>((*u)->size() - from, end - at);
        std::copy_n((*u)->begin() + static_cast<std::ptrdiff_t>(from), static_cast<std::ptrdiff_t>(take), out.begin() + static_cast<std::ptrdiff_t>(done));
        done += static_cast<std::size_t>(take);
        at += take;
    }
    return done;
}

Result<Drive> Drive::mount(const std::filesystem::path& cartridge, MountOptions options) {
    if (options.access != Access::read_only) {
        return std::unexpected(Error{Code::unsupported, "writing through the Drive is not built yet (phase 3): mount read-only", std::nullopt});
    }
    auto archive = sq::cart::Archive::open(cartridge, options.read);
    if (!archive) return std::unexpected(archive.error());
    return Drive(std::make_shared<File::State>(cartridge, std::move(*archive), options.cache_bytes));
}

Result<File> Drive::open(std::string_view path) const {
    auto e = state_->archive.find(path);
    if (!e) return std::unexpected(e.error());
    return File(state_, std::move(*e));
}

Result<Entry> Drive::stat(std::string_view path) const { return state_->archive.find(path); }

Result<std::vector<Entry>> Drive::list(std::string_view prefix) const {
    auto live = state_->archive.entries();
    if (!live) return std::unexpected(live.error());
    std::vector<Entry> out;
    const auto first = std::ranges::lower_bound(*live, prefix, {}, [](const Entry& e) -> std::string_view { return e.path; });
    for (auto it = first; it != live->end() && it->path.starts_with(prefix); ++it) out.push_back(*it);
    return out;
}

const sq::cart::Commit& Drive::commit() const noexcept { return state_->archive.commit(); }
const std::filesystem::path& Drive::path() const noexcept { return state_->path; }
CacheStats Drive::cache_stats() const { return state_->cache.stats(); }
void Drive::drop_cache() { state_->cache.clear(); }

}  // namespace sq::drive
