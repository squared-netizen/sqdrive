// SPDX-License-Identifier: MIT
//
// sqdrive: the Virtual Cartridge Drive on the command line.
//
//   sqdrive ls CART [PREFIX]           entries: size, codec, path
//   sqdrive stat CART PATH             one entry's record
//   sqdrive cat CART PATH [OFFSET [N]] bytes, verified, to stdout
//   sqdrive bench CART PATH            sequential and random reads, cold and warm
//
// Read-only for now; write, sync, commit and recover come with phase 3.

import std;
import sq.cart;
import sq.drive;

using namespace sq::drive;

namespace {

int usage() {
    std::println(std::cerr, "usage: sqdrive ls CART [PREFIX]\n"
                            "       sqdrive stat CART PATH\n"
                            "       sqdrive cat CART PATH [OFFSET [N]]\n"
                            "       sqdrive bench CART PATH");
    return 2;
}

int fail(const Error& e) {
    std::println(std::cerr, "sqdrive: {}{}{}", e.entry ? *e.entry : "", e.entry ? ": " : "", e.message);
    return 1;
}

std::string_view codec_name(sq::cart::Codec c) {
    switch (c) {
        case sq::cart::Codec::stored: return "stored";
        case sq::cart::Codec::zstd: return "zstd";
        case sq::cart::Codec::zstd_dict: return "zstd+dict";
        case sq::cart::Codec::chunked: return "chunked";
    }
    return "?";
}

std::optional<std::uint64_t> number(std::string_view t) {
    std::uint64_t v = 0;
    if (std::from_chars(t.data(), t.data() + t.size(), v).ec != std::errc{}) return std::nullopt;
    return v;
}

double mib_per_s(std::uint64_t bytes, std::chrono::steady_clock::duration d) {
    const double s = std::chrono::duration<double>(d).count();
    return s > 0 ? static_cast<double>(bytes) / (1 << 20) / s : 0;
}

int bench(Drive& drive, const File& f) {
    using clock = std::chrono::steady_clock;
    std::vector<std::byte> buf(64 * 1024);
    const auto sequential = [&] {
        const auto t0 = clock::now();
        std::uint64_t total = 0;
        for (std::uint64_t at = 0; at < f.size(); at += buf.size()) {
            auto got = f.read(at, std::span(buf));
            if (!got) return std::optional<std::pair<std::uint64_t, clock::duration>>{};
            total += *got;
        }
        return std::optional(std::pair{total, clock::now() - t0});
    };
    drive.drop_cache();
    const auto cold = sequential();
    const auto warm = sequential();
    if (!cold || !warm) return fail(Error{Code::io, "a read failed", f.entry().path});
    std::println("sequential, cold: {:.1f} MiB/s", mib_per_s(cold->first, cold->second));
    std::println("sequential, warm: {:.1f} MiB/s", mib_per_s(warm->first, warm->second));
    std::mt19937_64 rng(1);
    const int reads = 2000;
    const auto t0 = clock::now();
    for (int i = 0; i < reads; ++i) {
        const std::uint64_t at = f.size() > 4096 ? rng() % (f.size() - 4096) : 0;
        if (!f.read(at, std::span(buf).first(4096))) return fail(Error{Code::io, "a read failed", f.entry().path});
    }
    const auto us = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / reads;
    const auto s = drive.cache_stats();
    std::println("random 4 KiB:     {:.1f} µs a read", us);
    std::println("cache:            {} hits, {} misses, {} evictions, {} units ({} bytes) held", s.hits, s.misses, s.evictions, s.units, s.bytes);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2) return usage();
    const std::string_view cmd = args[0];
    auto drive = Drive::mount(std::filesystem::path(args[1]));
    if (!drive) return fail(drive.error());
    if (cmd == "ls") {
        auto list = drive->list(args.size() > 2 ? args[2] : std::string_view{});
        if (!list) return fail(list.error());
        for (const Entry& e : *list) std::println("{:>14} {:<9} {}", e.size, codec_name(e.codec), e.path);
        return 0;
    }
    if (args.size() < 3) return usage();
    if (cmd == "stat") {
        auto e = drive->stat(args[2]);
        if (!e) return fail(e.error());
        std::println("path:    {}\nsize:    {}\ncodec:   {}\nstored:  {}\ncommit:  {}", e->path, e->size, codec_name(e->codec), e->stored_size, e->commit);
        if (e->title) std::println("title:   {}", *e->title);
        if (e->media_type) std::println("type:    {}", *e->media_type);
        return 0;
    }
    auto f = drive->open(args[2]);
    if (!f) return fail(f.error());
    if (cmd == "cat") {
        const auto offset = args.size() > 3 ? number(args[3]) : std::optional<std::uint64_t>(0);
        const auto n = args.size() > 4 ? number(args[4]) : std::optional<std::uint64_t>(f->size());
        if (!offset || !n) return usage();
        std::vector<std::byte> buf(1 << 20);
        for (std::uint64_t at = *offset, left = *n; left > 0;) {
            auto got = f->read(at, std::span(buf).first(static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()))));
            if (!got) return fail(got.error());
            if (*got == 0) break;
            std::cout.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(*got));
            at += *got, left -= *got;
        }
        return 0;
    }
    if (cmd == "bench") return bench(*drive, *f);
    return usage();
}
