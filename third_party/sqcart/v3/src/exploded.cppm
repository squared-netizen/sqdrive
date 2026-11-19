// SPDX-License-Identifier: MIT
//
// sq.cart:exploded — a cartridge unpacked into a folder (§6.1): every live
// entry a file, its metadata in SQ-INF/entries.json, created so that the
// cartridge cannot write anywhere else. Packing a folder is the writer's
// (sq.cart.write's pack).
//
// Every file and folder is created by this code, relative to a folder it
// holds open, with O_NOFOLLOW and O_EXCL: a symbolic link planted in the
// target, or created by a name earlier in the cartridge, is never followed
// (the Zip Slip family, red-team R12).

module;

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module sq.cart:exploded;

import std;
import sq.data;
import :error;
import :json;
import :archive;

namespace sq::cart {

constexpr std::string_view entries_json = "SQ-INF/entries.json";

/// Windows cannot create these names (§4.1): refused by unpack everywhere,
/// so a cartridge unpacks on every system or on none.
[[nodiscard]] bool reserved_on_windows(std::string_view segment) {
    if (segment.ends_with('.') || segment.ends_with(' ')) return true;
    std::string base(segment.substr(0, segment.find('.')));
    std::ranges::transform(base, base.begin(), [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
    static constexpr std::string_view names[] = {"CON", "PRN", "AUX", "NUL"};
    if (std::ranges::find(names, base) != std::end(names)) return true;
    return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9';
}

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

/// The folder `rel` under `root`, made as needed, each step opened without
/// following a link.
Result<Fd> open_folder(int root, std::string_view rel, const std::string& entry) {
    Fd at(::dup(root));
    for (std::size_t from = 0; from < rel.size();) {
        std::size_t slash = rel.find('/', from);
        if (slash == std::string_view::npos) slash = rel.size();
        const std::string name(rel.substr(from, slash - from));
        if (::mkdirat(at.get(), name.c_str(), 0755) != 0 && errno != EEXIST) {
            return fail(Code::io, std::format("cannot make the folder {}: {}", name, std::strerror(errno)), entry);
        }
        Fd next(::openat(at.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next.ok()) {
            return fail(Code::path, std::format("{} is not a folder this can write into (a link?): {}", name, std::strerror(errno)), entry);
        }
        at = std::move(next);
        from = slash + 1;
    }
    return at;
}

Result<void> write_new(int folder, const std::string& name, std::span<const std::byte> bytes, const std::string& entry) {
    Fd fd(::openat(folder, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (!fd.ok()) return fail(Code::io, std::format("cannot create {}: {}", name, std::strerror(errno)), entry);
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd.get(), bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(Code::io, std::format("cannot write {}: {}", name, std::strerror(errno)), entry);
        done += static_cast<std::size_t>(n);
    }
    return {};
}

}  // namespace sq::cart

export namespace sq::cart {

/// The metadata of an entry as SQ-INF/entries.json holds it (§6.1): only
/// what differs from a plain file's (flags r, nothing else). Nothing when
/// there is nothing to say.
[[nodiscard]] std::optional<data::JsonValue> exploded_metadata(const Entry& e) {
    data::JsonValue::Object o;
    if (e.title) o.emplace("title", *e.title);
    if (e.author) o.emplace("author", *e.author);
    if (e.media_type) o.emplace("media_type", *e.media_type);
    if (e.created != 0) o.emplace("created", e.created);
    if (e.updated != 0) o.emplace("updated", e.updated);
    if (e.attributes) {
        auto a = data::parse_json(*e.attributes, {.ijson = true});
        if (a) o.emplace("attributes", std::move(a.value));
    }
    std::string letters;
    if (e.flags & flag::r) letters += 'r';
    if (e.flags & flag::w) letters += 'w';
    if (e.flags & flag::x) letters += 'x';
    if (e.flags & flag::mappable) letters += 'm';
    if (e.flags & flag::dictionary) letters += 'd';
    if (letters != "r") o.emplace("flags", letters);
    if (o.empty()) return std::nullopt;
    return data::JsonValue(std::move(o));
}

struct UnpackOptions {
    /// Write into a folder that has files already (never over one).
    bool merge = false;
};

/// Unpack a cartridge into `target` (§6.1): every live entry a file, read
/// and verified first; SQ-INF/entries.json for metadata a file system
/// cannot hold, in canonical JSON. Refuses, before writing anything, a path
/// Windows cannot create. Never follows a link; never makes a file
/// executable (2.0 §3.1, kept).
[[nodiscard]] Result<std::size_t> unpack(const Archive& cart, const std::filesystem::path& target, UnpackOptions options = {}) {
    auto live = cart.entries();
    if (!live) return std::unexpected(live.error());
    for (const Entry& e : *live) {
        for (std::size_t from = 0; from <= e.path.size();) {
            std::size_t slash = e.path.find('/', from);
            if (slash == std::string::npos) slash = e.path.size();
            if (reserved_on_windows(std::string_view(e.path).substr(from, slash - from))) {
                return fail(Code::path, "a name Windows cannot create (§4.1)", e.path);
            }
            from = slash + 1;
        }
        if (e.path == entries_json) return fail(Code::malformed, "SQ-INF/entries.json is the exploded form's, not an entry", e.path);
    }
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    Fd root(::open(target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!root.ok()) return fail(Code::io, std::format("cannot open {}: {}", target.string(), std::strerror(errno)));
    if (!options.merge && std::filesystem::directory_iterator(target, ec) != std::filesystem::directory_iterator()) {
        return fail(Code::io, target.string() + " is not empty (merge to write into it)");
    }
    data::JsonValue::Object metadata;
    std::size_t written = 0;
    for (const Entry& e : *live) {
        auto bytes = cart.read(e);
        if (!bytes) return std::unexpected(bytes.error());
        const std::size_t slash = e.path.rfind('/');
        auto folder = open_folder(root.get(), slash == std::string::npos ? std::string_view() : std::string_view(e.path).substr(0, slash), e.path);
        if (!folder) return std::unexpected(folder.error());
        if (auto r = write_new(folder->get(), e.path.substr(slash == std::string::npos ? 0 : slash + 1), *bytes, e.path); !r) {
            return std::unexpected(r.error());
        }
        if (auto m = exploded_metadata(e)) metadata.emplace(e.path, std::move(*m));
        ++written;
    }
    if (!metadata.empty()) {
        auto text = data::write_json(data::JsonValue(std::move(metadata)), {.canonical = true});
        if (!text) return fail(Code::json, text.error.message);
        auto folder = open_folder(root.get(), "SQ-INF", std::string(entries_json));
        if (!folder) return std::unexpected(folder.error());
        if (auto r = write_new(folder->get(), "entries.json", std::as_bytes(std::span(text.text)), std::string(entries_json)); !r) {
            return std::unexpected(r.error());
        }
    }
    return written;
}

}  // namespace sq::cart
