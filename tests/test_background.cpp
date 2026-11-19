// SPDX-License-Identifier: MIT
//
// Phase 4: commits on a thread of their own while writes go on; synced
// chunks out of memory; staging and memory bounds that hold while a file
// far larger than both streams in; commit_after; compaction on its own;
// and a crash with two journal segments, the older being committed.

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
fs::path dir;

fs::path cartridge(std::string_view name) {
    const fs::path p = dir / name;
    sq::cart::WriteOptions o;
    o.sync = false;
    auto w = sq::cart::Writer::create(p, o);
    static_cast<void>(w->put("SQ-INF/manifest.json", bytes(R"({"format":"squared-cartridge","format_version":3,"kind":"data","id":"t","version":"1"})")));
    static_cast<void>(w->commit());
    return p;
}

MountOptions writing(Policy policy) {
    MountOptions o;
    o.access = Access::read_write;
    o.staging = dir / "staging";
    o.commit = policy;
    o.write.sync = false;
    o.write.level = 1;
    o.compact_at = 0;
    return o;
}

std::optional<std::vector<std::byte>> committed(const fs::path& cart, std::string_view path, std::uint64_t at, std::uint64_t n) {
    auto a = sq::cart::Archive::open(cart);
    if (!a) return std::nullopt;
    auto e = a->find(path);
    if (!e) return std::nullopt;
    auto b = a->read_range(*e, at, n);
    if (!b) return std::nullopt;
    return *b;
}

bool checks(const fs::path& cart) {
    const auto r = sq::cart::check(cart);
    for (const auto& f : r.findings) std::println(std::cerr, "  {} {}", f.entry.value_or(""), f.message);
    return r.ok() && r.findings.empty();
}

/// The bytes at [at, at+n) of the streamed file: block k is pattern(256 KiB, k).
std::vector<std::byte> streamed(std::uint64_t at, std::uint64_t n) {
    constexpr std::uint64_t block = 256 * 1024;
    std::vector<std::byte> out;
    for (std::uint64_t k = at / block; out.size() < n; ++k) {
        const auto b = pattern(block, static_cast<unsigned>(k));
        const std::uint64_t from = out.empty() ? at - k * block : 0;
        const std::uint64_t take = std::min<std::uint64_t>(block - from, n - out.size());
        out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(from + take));
    }
    return out;
}

}  // namespace

int main() {
    dir = fs::temp_directory_path() / std::format("sqdrive-bg-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);

    // The gate: a file far larger than staging and memory streams in, and
    // both bounds hold, with commits in the background as it goes.
    {
        const auto cart = cartridge("stream.sq");
        auto o = writing(Policy::budget);
        o.staging_bytes = 24 * mib;
        o.commit_bytes = 8 * mib;
        o.memory_bytes = 4 * mib;
        constexpr std::uint64_t total = 256 * mib, block = 256 * 1024;
        std::uint64_t most_staged = 0, most_memory = 0;
        bool ok = true;
        {
            auto d = Drive::mount(cart, o);
            check(d.has_value(), d ? "mount, budget" : d.error().message);
            auto f = d->open("video.bin", mode::write | mode::create);
            for (std::uint64_t k = 0; k * block < total && ok; ++k) {
                ok = f->write(k * block, pattern(block, static_cast<unsigned>(k))).has_value();
                most_staged = std::max(most_staged, d->staged_bytes());
                most_memory = std::max(most_memory, d->memory_bytes());
            }
            check(ok, "256 MiB written, 256 KiB at a time");
            check(d->head().sequence > 4, std::format("commits happened as it went ({})", d->head().sequence));
            check(f->read(100 * mib + 17, 5000).value() == streamed(100 * mib + 17, 5000), "it reads back while staged and committed both");
            check(d->unmount().has_value(), "unmount commits the rest");
        }
        check(most_staged <= o.staging_bytes + 2 * mib, std::format("staged bytes stayed under staging_bytes ({} MiB at most)", most_staged / mib));
        check(most_memory <= o.memory_bytes + 2 * mib, std::format("memory stayed under memory_bytes ({} MiB at most)", most_memory / mib));
        bool same = true;
        std::mt19937_64 rng(5);
        for (int i = 0; i < 40 && same; ++i) {
            const std::uint64_t at = rng() % (total - 70000), n = rng() % 70000;
            same = committed(cart, "video.bin", at, n) == streamed(at, n);
        }
        check(same, "the cartridge holds every byte (40 random ranges)");
        auto a = sq::cart::Archive::open(cart);
        check(a && a->find("video.bin")->size == total && a->find("video.bin")->codec == sq::cart::Codec::chunked, "one chunked entry of 256 MiB");
        check(checks(cart), "check passes");
    }

    // Synced chunks leave memory and read back from the journal.
    {
        const auto cart = cartridge("memory.sq");
        auto o = writing(Policy::explicit_only);
        o.memory_bytes = 2 * mib;
        auto d = Drive::mount(cart, o);
        auto f = d->open("f.bin", mode::write | mode::create);
        for (std::uint64_t k = 0; k < 64; ++k) static_cast<void>(f->write(k * 256 * 1024, pattern(256 * 1024, static_cast<unsigned>(k))));
        check(d->staged_bytes() == 16 * mib && d->memory_bytes() <= 3 * mib, std::format("16 MiB staged, {} KiB of it in memory", d->memory_bytes() / 1024));
        check(f->read(0, 16 * mib).value() == streamed(0, 16 * mib), "and all of it reads back");
        check(f->write(5 * mib + 3, bytes("changed")).has_value(), "a synced chunk is changed again");
        auto expect = streamed(0, 16 * mib);
        std::ranges::copy(bytes("changed"), expect.begin() + static_cast<std::ptrdiff_t>(5 * mib + 3));
        check(f->read(0, 16 * mib).value() == expect, "and reads changed");
        check(d->commit().has_value() && committed(cart, "f.bin", 0, 16 * mib) == expect, "commit");
    }

    // commit_after: the oldest change waits no longer, with no call to the Drive.
    {
        const auto cart = cartridge("after.sq");
        auto o = writing(Policy::budget);
        o.commit_after = std::chrono::milliseconds(100);
        auto d = Drive::mount(cart, o);
        static_cast<void>(d->open("a.txt", mode::write | mode::create)->write(0, bytes("soon")));
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(d->head().sequence == 2 && committed(cart, "a.txt", 0, 4) == bytes("soon"), "committed after commit_after, unasked");
    }

    // Threads writing while commits run: their own files, and one file shared.
    {
        const auto cart = cartridge("threads.sq");
        auto o = writing(Policy::budget);
        o.commit_bytes = 2 * mib;
        auto d = Drive::mount(cart, o);
        static_cast<void>(d->open("shared.bin", mode::write | mode::create));
        std::vector<std::jthread> threads;
        std::atomic<bool> ok{true};
        for (unsigned t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                auto own = d->open(std::format("own-{}.bin", t), mode::write | mode::create);
                auto shared = d->open("shared.bin", mode::write);
                if (!own || !shared) ok = false;
                for (std::uint64_t k = 0; k < 32 && ok; ++k) {
                    const auto b = pattern(64 * 1024, t * 1000 + static_cast<unsigned>(k));
                    if (!own->write(k * 64 * 1024, b) || !shared->write((t * 32 + k) * 64 * 1024, b)) ok = false;
                }
            });
        }
        threads.clear();
        check(ok, "4 threads, 4 MiB each, written while commits run");
        check(d->wait().has_value() && d->commit().has_value(), "wait, and commit the rest");
        bool same = true;
        for (unsigned t = 0; t < 4; ++t) {
            for (std::uint64_t k = 0; k < 32; ++k) {
                const auto b = pattern(64 * 1024, t * 1000 + static_cast<unsigned>(k));
                same = same && committed(cart, std::format("own-{}.bin", t), k * 64 * 1024, 64 * 1024) == b &&
                       committed(cart, "shared.bin", (t * 32 + k) * 64 * 1024, 64 * 1024) == b;
            }
        }
        check(same, "every byte of every thread is committed");
        check(d->head().sequence > 3, "over several commits");
        check(checks(cart), "check passes");
    }

    // Compaction on its own, once enough of the file is unreachable.
    {
        const auto cart = cartridge("compact.sq");
        auto o = writing(Policy::explicit_only);
        o.compact_at = 0.5;
        o.compact_min_bytes = 2 * mib;
        auto d = Drive::mount(cart, o);
        auto f = d->open("f.bin", mode::write | mode::create);
        std::uint64_t biggest = 0;
        for (unsigned round = 0; round < 6; ++round) {
            static_cast<void>(f->write(0, pattern(4 * mib, round)));
            static_cast<void>(d->commit());
            biggest = std::max<std::uint64_t>(biggest, fs::file_size(cart));
        }
        check(fs::file_size(cart) < 10 * mib, std::format("the file was compacted ({} MiB, {} MiB at most)", fs::file_size(cart) / mib, biggest / mib));
        check(committed(cart, "f.bin", 0, 4 * mib) == pattern(4 * mib, 5), "and holds the last content");
        check(f->write(0, bytes("after")).has_value() && d->commit().has_value(), "writes go on after it");
        check(checks(cart), "check passes");
    }

    // A crash inside a commit while writes go on into the next segment:
    // before the commit landed, both segments replay; after, the older is
    // dropped and the newer replays on the new commit.
    for (const std::string point : {"before_commit", "after_commit"}) {
        const auto cart = cartridge(std::format("segments-{}.sq", point));
        std::cout.flush();
        const pid_t pid = ::fork();
        if (pid == 0) {
            auto d = Drive::mount(cart, writing(Policy::explicit_only));
            static_cast<void>(d->open("a.txt", mode::write | mode::create)->write(0, bytes("first")));
            ::setenv("SQDRIVE_CRASH", point.c_str(), 1);
            ::setenv("SQDRIVE_CRASH_DELAY_MS", "400", 1);
            std::jthread committer([&] { static_cast<void>(d->commit()); });  // stops the process, 400 ms in
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            static_cast<void>(d->open("b.txt", mode::write | mode::create)->write(0, bytes("second")));
            static_cast<void>(d->sync());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(1);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 86, std::format("{}: the child stopped inside its commit", point));
        const bool landed = point == "after_commit";
        check(committed(cart, "a.txt", 0, 5).has_value() == landed, std::format("{}: the commit {}", point, landed ? "landed" : "did not land"));
        check(!committed(cart, "b.txt", 0, 6), std::format("{}: b.txt was written during it, not in it", point));
        auto d = Drive::mount(cart, writing(Policy::explicit_only));
        check(d.has_value(), d ? std::format("{}: mount", point) : d.error().message);
        if (!d) continue;
        check(d->open("a.txt")->read(0, 5).value() == bytes("first") && d->open("b.txt")->read(0, 6).value() == bytes("second"),
              std::format("{}: both files are there", point));
        auto c = d->commit();
        check(c && c->sequence == (landed ? 3u : 2u), std::format("{}: one commit more, nothing committed twice", point));
        check(committed(cart, "a.txt", 0, 5) == bytes("first") && committed(cart, "b.txt", 0, 6) == bytes("second"), std::format("{}: both committed", point));
    }

    fs::remove_all(dir);
    if (failures) {
        std::println("FAIL background ({} failed)", failures);
        return 1;
    }
    std::println("ok   background");
    return 0;
}
