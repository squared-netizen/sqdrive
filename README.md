# sqdrive

The Virtual Cartridge Drive: the big-file interface for squared apps.
Mount a [Squared Cartridge 3.0](https://github.com/squared-netizen/sqcart)
and read (and, from phase 3, write) files of any size by range. sqcart is
the only thing that writes the format; the Drive builds on it and owns
staging, caching, commit policy and recovery.

- `sq.drive`: the C++23 module apps link (`import sq.drive;`).
- `sqdrive`: the command-line tool.

```cpp
import std;
import sq.drive;

auto drive = sq::drive::Drive::mount("world.sq", {.cache_bytes = 64 << 20});
if (!drive) { std::println(std::cerr, "{}", drive.error().message); return 1; }
auto map = drive->open("world/map.bin");
auto tile = map->read(offset, 4096);         // verified; only the chunks it covers
for (const auto& e : *drive->list("world/")) std::println("{} {}", e.size, e.path);
```

A `Drive` is thread-safe, and the `File`s it opens outlive it. Reads go
through a cache of decoded chunks (1 MiB each, or a whole entry that is
not chunked), least recently used first out, bounded in bytes and keyed by
content, so an unchanged chunk stays cached across commits.

```sh
sqdrive ls CART [PREFIX]
sqdrive stat CART PATH
sqdrive cat CART PATH [OFFSET [N]]
sqdrive bench CART PATH
```

## Building

```sh
make test
make CXX=/usr/lib/llvm-23/bin/clang++ test
```

Needs clang and libc++ with `import std;`; 23 is what sqterm and CI use.
Offline: sqcart's library and what it builds with are vendored in
`third_party/sqcart` (`tools/vendor_sqcart.sh ../sqcart` copies it again,
with its `PINNED`), and built first into `build/sqcart`. The tool links
only sqcart's reading archives, so it carries no compressor.

## Where it stands

The phases of the design:

1. **sqcart additions** (sqcart #8): renames, resizes, many ranges per
   commit, streamed puts, compaction under the writer's lock. Done.
2. **Read-only Drive**: this. On a 200 MB chunked file on a container's
   disk: sequential reads about 200 MiB/s; a random 4 KiB read that misses
   the cache costs about 3 ms, most of it SHA-256 over the 1 MiB chunk it
   verifies (a faster SHA-256 is the next thing to win).
3. **Writing and the journal**: `write`, `sync`, `commit`, replay after a
   crash.
4. **Background commits and backpressure.**
5. **Hosts**: a pkg recipe for sqterm, the Android hook that commits on
   `onStop`.

## Licence

MIT. Security issues: see [SECURITY.md](SECURITY.md).
