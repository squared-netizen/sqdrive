// SPDX-License-Identifier: MIT
//
// Read-write mounts: writes, resizes, creates, removes and renames, read
// back before and after a commit and checked against a model; chunks no
// write touched named again; commit policies and backpressure; the lock.
// And crashes, in a child process stopped dead: unsynced changes lost,
// synced ones replayed, a torn tail ignored, a commit that did or did not
// land, and a cartridge another writer moved on (Code::conflict).

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import sq.cart;
import sq.cart.write;
import sq.drive;

using namespace sq::drive;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, std::string_view what, std::source_location at = std::source_location::current()) {
    if (!ok) {
        ++failures;
        std::println(std::cerr, "FAIL {}:{}  {}", at.file_name(), at.line(), what);
    }
}

std::vector<std::byte> bytes(std::string_view s) {
    const auto b = std::as_bytes(std::span(s));
    return {b.begin(), b.end()};
}

std::vector<std::byte> pattern(std::size_t n, unsigned seed) {
    std::vector<std::byte> b(n);
    std::uint32_t x = seed;
    for (auto& v : b) {
        x = x * 1664525u + 1013904223u;
        v = static_cast<std::byte>(x >> 24);
    }
    return b;
}

constexpr std::uint64_t mib = sq::cart::chunk_size;
const std::uint16_t rw = sq::cart::flag::r | sq::cart::flag::w;

fs::path dir;

/// A cartridge with a manifest and `entries` (writable), committed.
fs::path cartridge(std::string_view name, std::initializer_list<std::tuple<std::string, std::vector<std::byte>, bool>> entries) {
    const fs::path p = dir / name;
    fs::remove(p);
    sq::cart::WriteOptions o;
    o.sync = false;
    o.level = 1;
    auto w = sq::cart::Writer::create(p, o);
    static_cast<void>(w->put("SQ-INF/manifest.json", bytes(R"({"format":"squared-cartridge","format_version":3,"kind":"data","id":"t","version":"1"})")));
    for (const auto& [path, content, chunked] : entries) {
        static_cast<void>(w->put(path, content, {.flags = rw, .store = chunked ? sq::cart::Store::chunked : sq::cart::Store::automatic}));
    }
    auto c = w->commit();
    check(c.has_value(), c ? "make a cartridge" : c.error().message);
    return p;
}

MountOptions writing(Policy policy = Policy::explicit_only) {
    MountOptions o;
    o.access = Access::read_write;
    o.staging = dir / "staging";
    o.commit = policy;
    o.write.sync = false;
    o.write.level = 1;
    return o;
}

/// What the cartridge itself holds at `path` (through sq.cart, not the Drive).
std::optional<std::vector<std::byte>> committed(const fs::path& cart, std::string_view path) {
    auto a = sq::cart::Archive::open(cart);
    if (!a) return std::nullopt;
    auto e = a->find(path);
    if (!e) return std::nullopt;
    auto b = a->read(*e);
    if (!b) return std::nullopt;
    return *b;
}

bool checks(const fs::path& cart) {
    const auto r = sq::cart::check(cart);
    for (const auto& f : r.findings) std::println(std::cerr, "  {} {}", f.entry.value_or(""), f.message);
    return r.ok() && r.findings.empty();
}

/// Run `body` in a child process, which ends as it ends it (std::_Exit:
/// no unmount, no destructors, as a crash) or with 0. Its exit status.
int in_child(const std::function<void()>& body) {
    std::cout.flush();
    const pid_t pid = ::fork();
    if (pid == 0) {
        body();
        std::_Exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// Random writes and resizes on one file, the same in a model.
void random_edits(File& f, std::vector<std::byte>& model, unsigned seed, int n, std::uint64_t span) {
    std::mt19937_64 rng(seed);
    for (int k = 0; k < n; ++k) {
        if (rng() % 5 == 0) {
            const std::uint64_t size = rng() % span;
            static_cast<void>(f.resize(size));
            model.resize(static_cast<std::size_t>(size));
        } else {
            const std::uint64_t at = rng() % (model.size() + mib / 2), len = rng() % (mib + 5000);
            const auto data = pattern(static_cast<std::size_t>(len), static_cast<unsigned>(rng()));
            static_cast<void>(f.write(at, data));
            if (at + len > model.size()) model.resize(static_cast<std::size_t>(at + len));
            std::ranges::copy(data, model.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
}

}  // namespace

int main() {
    dir = fs::temp_directory_path() / std::format("sqdrive-write-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);

    // Edits of a chunked file, read back before and after the commit.
    {
        auto model = pattern(mib * 4 + 777, 1);
        const auto cart = cartridge("edit.sq", {{"map.bin", model, true}, {"note.txt", bytes("a note"), false}});
        std::vector<sq::cart::Chunk> before;
        {
            auto a = sq::cart::Archive::open(cart);
            before = a->chunks(*a->find("map.bin")).value();
        }
        {
            auto d = Drive::mount(cart, writing());
            check(d.has_value(), d ? "mount read-write" : d.error().message);
            if (!d) return 1;
            auto f = d->open("map.bin", mode::write);
            check(f.has_value(), "open for writing");
            const auto patch = pattern(3000, 2);
            check(f->write(mib - 1000, patch).has_value(), "a write across chunks 0 and 1");
            std::ranges::copy(patch, model.begin() + static_cast<std::ptrdiff_t>(mib - 1000));
            check(f->read(0, model.size()).value() == model, "reads see it before the commit");
            check(sq::cart::Archive::open(cart).has_value() && committed(cart, "map.bin") != model, "and the cartridge does not, yet");
            auto c = d->commit();
            check(c && c->sequence == 2, c ? "commit" : c.error().message);
            check(f->read(0, model.size()).value() == model, "reads see it after the commit");
            check(d->staged_bytes() == 0, "nothing staged after it");
            auto unchanged = d->commit();
            check(unchanged && unchanged->sequence == 2, "a commit of nothing is no commit");
        }
        check(committed(cart, "map.bin") == model, "the cartridge holds it");
        auto a = sq::cart::Archive::open(cart);
        const auto after = a->chunks(*a->find("map.bin")).value();
        check(after[2].offset == before[2].offset && after[3].offset == before[3].offset && after[4].offset == before[4].offset,
              "chunks no write touched are named again");
        check(after[0].offset != before[0].offset && after[1].offset != before[1].offset, "touched ones are written");
        check(checks(cart), "check passes");
    }

    // A model: random writes and resizes of a chunked and a small file, many commits.
    {
        auto big = pattern(mib * 2 + 5, 3), small = pattern(9000, 4);
        const auto cart = cartridge("model.sq", {{"big.bin", big, true}, {"small.bin", small, false}});
        bool same = true;
        for (unsigned round = 0; round < 4; ++round) {
            auto d = Drive::mount(cart, writing());
            auto b = d->open("big.bin", mode::write);
            auto s = d->open("small.bin", mode::write);
            random_edits(*b, big, 10 + round, 15, 5 * mib);
            random_edits(*s, small, 20 + round, 15, 3 * mib);
            same = same && b->read(0, big.size()).value() == big && s->read(0, small.size()).value() == small;
            if (round % 2 == 0) static_cast<void>(d->sync());
            same = same && d->commit().has_value();
            same = same && committed(cart, "big.bin") == big && committed(cart, "small.bin") == small;
        }
        check(same, "60 random writes and resizes, 4 commits, match the model");
        check(checks(cart), "check passes");
    }

    // Create, remove, rename (a swap too); a file past 64 MiB is written chunked.
    {
        const auto cart = cartridge("names.sq", {{"a.txt", bytes("A"), false}, {"b.txt", bytes("B"), false}, {"big.bin", pattern(2 * mib, 5), true},
                                                 {"gone.txt", bytes("bye"), false}});
        auto o = writing();
        o.read.limits.max_unchunked_size = 3 * mib;  // so "past the limit" is small enough to test
        const auto huge = pattern(4 * mib + 3, 6);
        {
            auto d = Drive::mount(cart, o);
            auto n = d->open("new/file.txt", mode::write | mode::create);
            check(n.has_value() && n->size() == 0, "create a file");
            check(n->write(0, bytes("fresh")).has_value(), "write it");
            check(d->rename("a.txt", "tmp").has_value() && d->rename("b.txt", "a.txt").has_value() && d->rename("tmp", "b.txt").has_value(), "swap a and b");
            check(d->rename("big.bin", "moved/big.bin").has_value(), "rename a chunked file");
            check(d->remove("gone.txt").has_value(), "remove a file");
            check(!d->open("gone.txt"), "it is gone");
            auto h = d->open("huge.bin", mode::write | mode::create);
            check(h->write(0, huge).has_value(), "a new file past the unchunked limit");
            auto names = d->list();
            std::vector<std::string> paths;
            for (const auto& e : *names) paths.push_back(e.path);
            check(paths == std::vector<std::string>{"SQ-INF/manifest.json", "a.txt", "b.txt", "huge.bin", "moved/big.bin", "new/file.txt"}, "list shows what is staged");
            check(!d->rename("SQ-INF/manifest.json", "m"), "the manifest stays");
            check(!d->open("../x", mode::write | mode::create), "a bad path is refused");
            check(d->commit().has_value(), "commit");
        }
        check(committed(cart, "a.txt") == bytes("B") && committed(cart, "b.txt") == bytes("A"), "the swap landed");
        check(committed(cart, "moved/big.bin") == pattern(2 * mib, 5) && !committed(cart, "big.bin"), "the rename landed");
        check(!committed(cart, "gone.txt") && committed(cart, "new/file.txt") == bytes("fresh"), "the remove and the new file landed");
        auto a = sq::cart::Archive::open(cart);
        check(a->find("huge.bin")->codec == sq::cart::Codec::chunked && committed(cart, "huge.bin") == huge, "the big new file is chunked");
        check(checks(cart), "check passes");
    }

    // Files without w are not changed; a second writer is refused.
    {
        const auto cart = dir / "readonly.sq";
        {
            sq::cart::WriteOptions o;
            o.sync = false;
            auto w = sq::cart::Writer::create(cart, o);
            static_cast<void>(w->put("SQ-INF/manifest.json", bytes(R"({"format":"squared-cartridge","format_version":3,"kind":"data","id":"t","version":"1"})")));
            static_cast<void>(w->put("fixed.txt", bytes("fixed")));  // flags r only
            static_cast<void>(w->commit());
        }
        auto d = Drive::mount(cart, writing());
        auto f = d->open("fixed.txt", mode::write);
        check(!f && f.error().code == Code::unsupported, "an entry without w is not opened for writing");
        check(!d->remove("fixed.txt"), "nor removed");
        check(d->open("fixed.txt").has_value(), "but it reads");
        auto other = Drive::mount(cart, writing());
        check(!other && other.error().code == Code::busy, "a second read-write mount gets Code::busy");
        check(Drive::mount(cart).has_value(), "a read-only one is fine");
    }

    // Policies: budget commits on its own; explicit_only refuses past staging_bytes.
    {
        const auto cart = cartridge("policy.sq", {{"f.bin", pattern(10, 7), false}});
        {
            auto o = writing(Policy::budget);
            o.commit_bytes = 2 * mib;
            auto d = Drive::mount(cart, o);
            auto f = d->open("f.bin", mode::write);
            for (int i = 0; i < 5; ++i) static_cast<void>(f->write(static_cast<std::uint64_t>(i) * mib, pattern(mib, 8)));
            check(d->wait().has_value() && d->head().sequence > 1, "budget: a commit past commit_bytes, unasked");
        }
        {
            auto o = writing(Policy::explicit_only);
            o.staging_bytes = 2 * mib;
            auto d = Drive::mount(cart, o);
            auto f = d->open("f.bin", mode::write);
            Result<void> r;
            for (int i = 0; i < 5 && r; ++i) r = f->write(static_cast<std::uint64_t>(i) * mib, pattern(mib, 9));
            check(!r && r.error().code == Code::limit, "explicit_only: past staging_bytes, Code::limit");
            check(d->commit().has_value() && f->write(0, bytes("ok")).has_value(), "and after a commit, writes go on");
        }
    }

    // Crashes.
    const auto crash = [](const fs::path& cart, std::function<void(Drive&)> body, std::optional<std::string> point = std::nullopt) {
        return in_child([&] {
            if (point) ::setenv("SQDRIVE_CRASH", point->c_str(), 1);
            auto d = Drive::mount(cart, writing());
            if (!d) std::_Exit(3);
            body(*d);
            std::_Exit(0);  // a crash: no unmount
        });
    };
    {
        const auto cart = cartridge("crash1.sq", {{"f.txt", bytes("before"), false}});
        crash(cart, [](Drive& d) {
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("synced")));
            static_cast<void>(d.sync());
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("LOST!!")));
        });
        auto d = Drive::mount(cart, writing());
        check(d.has_value(), d ? "mount after a crash" : d.error().message);
        check(d->open("f.txt")->read(0, 6).value() == bytes("synced"), "synced changes are replayed; unsynced ones are lost");
        check(committed(cart, "f.txt") == bytes("before"), "and the cartridge was not touched");
        check(d->commit().has_value(), "commit them");
        d = std::unexpected(Error{});
        check(committed(cart, "f.txt") == bytes("synced"), "now it holds them");
    }
    {
        const auto cart = cartridge("torn.sq", {{"f.txt", bytes("before"), false}});
        fs::path journal;
        crash(cart, [](Drive& d) {
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("synced")));
            static_cast<void>(d.sync());
        });
        {
            auto d = Drive::mount(cart, writing());
            journal = d->journal_path();
            check(d->open("f.txt")->read(0, 6).value() == bytes("synced"), "replayed");
        }  // explicit_only: unmount syncs, keeps the journal
        {
            std::ofstream(journal, std::ios::binary | std::ios::app) << "\x40\x00\x00\x00\x02torn half a record";
        }
        auto d = Drive::mount(cart, writing());
        check(d && d->open("f.txt")->read(0, 6).value() == bytes("synced"), "a torn tail is ignored");
    }
    {
        const auto cart = cartridge("before.sq", {{"f.txt", bytes("before"), false}});
        const int status = crash(cart, [](Drive& d) {
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("commit")));
            static_cast<void>(d.commit());
        }, "before_commit");
        check(status == 86, "the child stopped before its commit");
        check(committed(cart, "f.txt") == bytes("before"), "the commit did not land");
        auto d = Drive::mount(cart, writing());
        check(d && d->open("f.txt")->read(0, 6).value() == bytes("commit"), "its changes are replayed");
        check(d->commit().has_value() && committed(cart, "f.txt") == bytes("commit"), "and commit");
    }
    {
        const auto cart = cartridge("after.sq", {{"f.txt", bytes("before"), false}});
        const int status = crash(cart, [](Drive& d) {
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("landed")));
            static_cast<void>(d.commit());
        }, "after_commit");
        check(status == 86, "the child stopped after its commit");
        check(committed(cart, "f.txt") == bytes("landed"), "the commit landed");
        auto d = Drive::mount(cart, writing());
        check(d.has_value(), d ? "mount knows it landed" : d.error().message);
        check(d->staged_bytes() == 0 && d->commit()->sequence == 2, "nothing is replayed or committed twice");
    }
    {
        const auto cart = cartridge("conflict.sq", {{"f.txt", bytes("before"), false}});
        crash(cart, [](Drive& d) {
            static_cast<void>(d.open("f.txt", mode::write)->write(0, bytes("mine..")));
            static_cast<void>(d.sync());
        });
        {
            sq::cart::WriteOptions o;
            o.sync = false;
            auto w = sq::cart::Writer::open(cart, o);
            static_cast<void>(w->put("other.txt", bytes("theirs")));
            static_cast<void>(w->commit());
        }
        auto d = Drive::mount(cart, writing());
        check(!d && d.error().code == Code::conflict, "a cartridge moved on by another writer: Code::conflict");
        auto o = writing();
        o.discard_journal = true;
        auto d2 = Drive::mount(cart, o);
        check(d2 && d2->open("f.txt")->read(0, 6).value() == bytes("before"), "discard_journal sets it aside");
        bool aside = false;
        for (const auto& e : fs::directory_iterator(d2->journal_path().parent_path())) aside = aside || e.path().filename().string().starts_with("journal.conflict-");
        check(aside, "and keeps it, as journal.conflict-<time>");
    }
    {
        // A model across a crash: random edits, synced, the child stopped; the same edits here.
        auto content = pattern(mib * 3 + 1, 11);
        const auto cart = cartridge("modelcrash.sq", {{"m.bin", content, true}});
        crash(cart, [](Drive& d) {
            auto f = d.open("m.bin", mode::write);
            auto unused = pattern(mib * 3 + 1, 11);
            random_edits(*f, unused, 77, 25, 6 * mib);
            static_cast<void>(d.rename("m.bin", "n.bin"));
            static_cast<void>(d.sync());
        });
        auto f = std::vector<std::byte>(content);
        {
            // Replay the same edits on a scratch Drive to get the model.
            const auto scratch = cartridge("modelcrash-model.sq", {{"m.bin", content, true}});
            auto d = Drive::mount(scratch, writing());
            auto file = d->open("m.bin", mode::write);
            random_edits(*file, f, 77, 25, 6 * mib);
        }
        auto d = Drive::mount(cart, writing());
        check(d && d->open("n.bin")->read(0, f.size() + 10).value() == f, "25 random edits and a rename, synced, survive a crash");
        check(d->commit().has_value() && committed(cart, "n.bin") == f && !committed(cart, "m.bin"), "and commit");
        check(checks(cart), "check passes");
    }

    fs::remove_all(dir);
    if (failures) {
        std::println("FAIL write ({} failed)", failures);
        return 1;
    }
    std::println("ok   write");
    return 0;
}
