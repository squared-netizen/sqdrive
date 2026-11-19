// SPDX-License-Identifier: MIT
//
// Read-only mounts: every read matches what sq.cart.write put, at random
// offsets and across chunk boundaries; the cache hits, evicts within its
// budget, and keeps nothing it cannot hold; files outlive their Drive;
// threads read at once.

import std;
import sq.cart;
import sq.cart.write;
import sq.drive;

using namespace sq::drive;

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

}  // namespace

int main() {
    const std::uint64_t mib = sq::cart::chunk_size;
    const auto dir = std::filesystem::temp_directory_path() / std::format("sqdrive-read-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    const auto cart = dir / "world.sq";
    const auto big = pattern(mib * 5 + 1234, 1);
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "The quick brown fox jumps over the lazy dog. ";
    const auto small = bytes(text);
    const auto raw = pattern(30000, 2);
    {
        sq::cart::WriteOptions o;
        o.sync = false;
        o.level = 1;
        auto w = sq::cart::Writer::create(cart, o);
        check(w.has_value(), "create a cartridge");
        if (!w) return 1;
        static_cast<void>(w->put("SQ-INF/manifest.json", bytes(R"({"format":"squared-cartridge","format_version":3,"kind":"data","id":"t","version":"1"})")));
        static_cast<void>(w->put("maps/world.bin", big, {.store = sq::cart::Store::chunked}));
        static_cast<void>(w->put("text/fox.txt", small));
        static_cast<void>(w->put("raw/noise.bin", raw));
        auto c = w->commit();
        check(c.has_value(), c ? "commit" : c.error().message);
    }

    auto drive = Drive::mount(cart, {.cache_bytes = 3 * mib});
    check(drive.has_value(), "mount read-only");
    if (!drive) return 1;
    {
        auto w = drive->open("text/fox.txt", mode::write);
        check(!w && w.error().code == Code::unsupported, "a read-only mount refuses to open for writing");
        check(!drive->remove("text/fox.txt") && !drive->rename("text/fox.txt", "x"), "and to remove or rename");
    }
    check(!Drive::mount(dir / "missing.sq"), "a missing cartridge is refused");

    // list, stat, open.
    auto all = drive->list();
    check(all && all->size() == 4, "list: four entries");
    auto maps = drive->list("maps/");
    check(maps && maps->size() == 1 && maps->front().path == "maps/world.bin", "list a prefix");
    check(drive->stat("text/fox.txt").value().size == small.size(), "stat");
    auto missing = drive->open("nothing");
    check(!missing && missing.error().code == Code::not_found, "opening nothing: not_found");

    // Reads match, across chunk boundaries and at the edges.
    auto f = drive->open("maps/world.bin");
    check(f && f->size() == big.size(), "open a chunked file");
    std::mt19937_64 rng(7);
    bool same = true;
    for (int i = 0; i < 300 && same; ++i) {
        const std::uint64_t at = rng() % big.size(), n = rng() % (2 * mib);
        auto got = f->read(at, n);
        const std::uint64_t expect = std::min<std::uint64_t>(n, big.size() - at);
        same = got && got->size() == expect && std::equal(got->begin(), got->end(), big.begin() + static_cast<std::ptrdiff_t>(at));
    }
    check(same, "300 random ranges of a chunked file read what was put");
    check(f->read(big.size(), 10).value().empty() && f->read(big.size() + 99, 10).value().empty(), "at or past the end: nothing");
    check(f->read(mib - 3, 6).value() == std::vector<std::byte>(big.begin() + static_cast<std::ptrdiff_t>(mib - 3), big.begin() + static_cast<std::ptrdiff_t>(mib + 3)),
          "a read across a chunk boundary");
    check(drive->open("text/fox.txt")->read(0, small.size()).value() == small, "a compressed file reads whole");
    check(drive->open("raw/noise.bin")->read(100, 50).value() == std::vector<std::byte>(raw.begin() + 100, raw.begin() + 150), "a stored file, a range");

    // The cache: within its budget, hits when warm.
    {
        const auto s = drive->cache_stats();
        check(s.bytes <= 3 * mib, std::format("the cache stays within its budget ({} bytes)", s.bytes));
        check(s.evictions > 0, "and evicts to stay there");
        drive->drop_cache();
        std::vector<std::byte> buf(4096);
        static_cast<void>(f->read(0, std::span(buf)));
        const auto cold = drive->cache_stats();
        static_cast<void>(f->read(100, std::span(buf)));
        const auto warm = drive->cache_stats();
        check(warm.hits == cold.hits + 1 && warm.misses == cold.misses, "a second read of a chunk hits the cache");
        check(warm.units == 1 && warm.bytes == mib, "one chunk held");
    }
    {
        auto none = Drive::mount(cart, {.cache_bytes = 0});
        auto g = none->open("maps/world.bin");
        check(g->read(0, 100).value() == std::vector<std::byte>(big.begin(), big.begin() + 100), "with no cache, reads still work");
        check(none->cache_stats().units == 0, "and nothing is kept");
    }

    // A file outlives its Drive.
    std::optional<File> kept;
    {
        auto d = Drive::mount(cart);
        kept = *d->open("text/fox.txt");
    }
    check(kept->read(4, 5).value() == bytes("quick"), "a file outlives the Drive it came from");

    // Threads.
    {
        std::atomic<bool> ok{true};
        std::vector<std::jthread> threads;
        for (unsigned t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937_64 r(t);
                for (int i = 0; i < 100; ++i) {
                    const std::uint64_t at = r() % big.size(), n = r() % 70000;
                    auto got = f->read(at, n);
                    if (!got || !std::equal(got->begin(), got->end(), big.begin() + static_cast<std::ptrdiff_t>(at))) ok = false;
                }
            });
        }
        threads.clear();
        check(ok, "8 threads reading at once read what was put");
    }

    // Damage is refused, not returned.
    {
        auto bad = dir / "bad.sq";
        std::filesystem::copy_file(cart, bad);
        const auto chunk0 = sq::cart::Archive::open(bad)->chunks(*sq::cart::Archive::open(bad)->find("maps/world.bin")).value().front();
        {
            std::fstream io(bad, std::ios::in | std::ios::out | std::ios::binary);
            io.seekp(static_cast<std::streamoff>(chunk0.offset + 10));
            io.put('\x55');
        }
        auto d = Drive::mount(bad);
        auto r = d->open("maps/world.bin")->read(0, 100);
        check(!r && r.error().code == Code::integrity, "a damaged chunk is refused");
        check(d->open("maps/world.bin")->read(2 * mib, 100).has_value(), "chunks that are whole still read");
    }

    std::filesystem::remove_all(dir);
    if (failures) {
        std::println("FAIL read ({} failed)", failures);
        return 1;
    }
    std::println("ok   read");
    return 0;
}
