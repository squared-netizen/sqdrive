// SPDX-License-Identifier: MIT
//
// A folder and a cartridge, both ways (pack, explode): sqcart's exploded
// form through the Drive's API, a file past 64 MiB never held whole, what is
// left out, and the Drive reading what pack made.

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

void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
                          std::format("sqdrive-folders-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path src = root / "workspace";
    write(src / "SQ-INF/manifest.json",
          R"({"format":"squared-cartridge","format_version":3,"kind":"workspace","id":"dev.me.demo","version":"0.1.0"})");
    write(src / "src/main.cpp", "int main() {}\n");
    write(src / "build/app.o", "made, not kept");
    write(src / ".git/HEAD", "ref: refs/heads/main");
    const std::uint64_t big = (std::uint64_t{64} << 20) + 5;
    {
        fs::create_directories(src / "assets");
        std::ofstream out(src / "assets/world.bin", std::ios::binary);
        std::vector<char> block(1 << 20);
        for (std::uint64_t at = 0; at < big; at += block.size()) {
            for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>((at + i) * 40503u >> 7);
            out.write(block.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(block.size(), big - at)));
        }
    }

    const fs::path cart = root / "demo.sq";
    sq::cart::WriteOptions quick;
    quick.sync = false;
    quick.level = 3;
    auto packed = pack(src, cart, {.leave_out = {"build"}, .write = quick});
    check(packed.has_value(), packed ? "pack" : packed.error().message);
    check(!pack(src, cart).has_value(), "pack refuses a cartridge that is there");

    // The Drive reads what pack made: build/ and .git/ left out, the big file chunked.
    {
        auto drive = Drive::mount(cart);
        check(drive.has_value(), "the packed cartridge mounts");
        if (drive) {
            check(drive->stat("src/main.cpp").has_value() && !drive->stat("build/app.o") && !drive->stat(".git/HEAD"),
                  "build/ (left out) and .git/ (a dot folder) not packed");
            auto e = drive->stat("assets/world.bin");
            check(e && e->codec == sq::cart::Codec::chunked && e->size == big, "the big file, chunked");
        }
    }

    const fs::path out = root / "exploded";
    auto files = explode(cart, out);
    check(files.has_value(), files ? "explode" : files.error().message);
    check(files && *files == 3, "three files: the manifest, main.cpp, world.bin");
    check(read(out / "src/main.cpp") == "int main() {}\n", "a small file, the same");
    check(fs::file_size(out / "assets/world.bin") == big && read(out / "assets/world.bin") == read(src / "assets/world.bin"),
          "the big file, the same bytes");
    check(!fs::exists(out / "assets/world.bin.part"), "no .part left");
    check(!explode(cart, out).has_value(), "explode refuses a folder with files, unless merge");

    fs::remove_all(root);
    if (failures == 0) std::println("ok   folders");
    return failures == 0 ? 0 : 1;
}
