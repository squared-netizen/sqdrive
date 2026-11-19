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
using namespace sq::drive;

auto drive = Drive::mount("world.sq", {.access = Access::read_write, .staging = app_dir / "drive"});
if (!drive) { std::println(std::cerr, "{}", drive.error().message); return 1; }
auto map = drive->open("world/map.bin", mode::write);
auto tile = map->read(offset, 4096);     // verified; only the chunks it covers
map->write(offset, patch);               // staged in memory: 1 MiB chunks
map->sync();                             // in the journal, flushed: survives a crash
drive->rename("world/map.bin", "maps/world.bin");  // nothing copied
drive->commit();                         // one sqcart commit of everything
drive->unmount();                        // commits (or syncs) as the policy says
```

A `Drive` is thread-safe, and the `File`s it opens outlive it. Reads go
through a cache of decoded chunks (1 MiB each, or a whole entry that is
not chunked), least recently used first out, bounded in bytes and keyed by
content, so an unchanged chunk stays cached across commits.

**Writing.** A read-write mount holds sqcart's writer lock until it
unmounts (a second one gets `Code::busy`). Writes change 1 MiB chunks in
memory; `sync()` appends what changed to a journal in the staging folder
(one per cartridge, never beside it) and flushes it; a commit hands
everything to sqcart as one commit, naming again every chunk no write
touched, and starts the journal again. Entries without the `w` flag are
not changed (§4.2). The policy says when to commit on its own:
`budget` (past `commit_bytes`, or `commit_after` since the first change,
and at unmount), `explicit_only`, or `on_unmount`. Past `staging_bytes`, a
write commits first (`budget`) or gets `Code::limit`.

**After a crash**, the next read-write mount reads the journal up to its
last sync whose checksum passes (a torn tail is dropped) and:

| The cartridge is | The Drive |
| --- | --- |
| at the journal's commit | replays what was synced; it commits as the policy says |
| one commit on, with the time the journal wrote down before committing | knows its commit landed; drops the journal |
| anywhere else (another writer committed) | refuses with `Code::conflict`, or, with `discard_journal`, sets the journal aside as `journal.conflict-<time>` |

```sh
sqdrive ls CART [PREFIX]
sqdrive stat CART PATH
sqdrive cat CART PATH [OFFSET [N]]
sqdrive bench CART PATH
sqdrive write CART PATH [OFFSET] [--keep]     # stdin into a file
sqdrive truncate CART PATH SIZE [--keep]
sqdrive rm CART PATH [--keep]
sqdrive mv CART FROM TO [--keep]
sqdrive commit CART                            # what --keep left in the journal
sqdrive recover CART --discard
```

## Building

```sh
make test
make CXX=/usr/lib/llvm-23/bin/clang++ test
```

Needs clang and libc++ with `import std;`; 23 is what sqterm and CI use.
Offline: sqcart's library and what it builds with are vendored in
`third_party/sqcart` (`tools/vendor_sqcart.sh ../sqcart` copies it again,
with its `PINNED`), and built first into `build/sqcart`.

## Where it stands

The phases of the design:

1. **sqcart additions** (sqcart #8): renames, resizes, many ranges per
   commit, streamed puts, compaction under the writer's lock. Done.
2. **Read-only Drive** (sqdrive #1). On a 200 MB chunked file on a
   container's disk: sequential reads about 200 MiB/s; a random 4 KiB read
   that misses the cache costs about 3 ms, most of it SHA-256 over the
   1 MiB chunk it verifies (a faster SHA-256 is the next thing to win).
3. **Writing and the journal**: this. Changes are held in memory until
   they are committed (synced ones in the journal too); phase 4 lets
   synced chunks leave memory.
4. **Background commits and backpressure.**
5. **Hosts**: a pkg recipe for sqterm, the Android hook that commits on
   `onStop`.

## Licence

MIT. Security issues: see [SECURITY.md](SECURITY.md).
