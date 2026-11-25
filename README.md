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
memory; `sync()` appends what changed to the journal in the staging folder
(one per cartridge, never beside it) and flushes it, and the synced chunks
leave memory: they are read back from the journal. Past `memory_bytes` a
write syncs first, so memory stays bounded however much is staged. Entries
without the `w` flag are not changed (§4.2).

**Committing.** A commit freezes what is staged, starts a new journal
segment for the writes that go on meanwhile, and hands the frozen changes
to sqcart as one commit, naming again every chunk no write touched; only a
write to a path being committed waits for it. With the `budget` policy
(the default) commits run on a thread of their own, past `commit_bytes` or
once the oldest change is `commit_after` old, and at unmount; with
`explicit_only` only on `commit()`; with `on_unmount` also at unmount. Past
`staging_bytes` a write waits for a commit (`budget`) or gets `Code::limit`.
Files the Drive makes are stored chunked, so later commits write only the
chunks they change. After a commit with nothing else staged, the Drive
compacts the cartridge (§10.4) when `compact_min_bytes` and `compact_at` of
it are unreachable; writers wait meanwhile, readers do not. `wait()` waits
for background commits and reports one that failed.

**After a crash**, the next read-write mount reads each journal segment up
to its last sync whose checksum passes (a torn tail is dropped) and:

| The cartridge is | The Drive |
| --- | --- |
| at the segments' commit | replays them all; it commits as the policy says |
| one commit on, with the time a segment wrote down before committing | knows that commit landed: drops that segment and those before it, replays the ones after |
| anywhere else (another writer committed) | refuses with `Code::conflict`, or, with `discard_journal`, sets the segments aside as `journal.conflict-<time>.<n>` |

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
sqdrive pack DIR CART [--keep .NAME]... [--leave-out PATH]...   # a folder into a new cartridge
sqdrive explode CART DIR [--merge]             # a cartridge out as a folder
sqdrive recover CART --discard
sqdrive --help | --version                     # the commands; the release
```

**Folders.** `pack(folder, cartridge, {keep, leave_out})` makes a new
cartridge of an exploded one (a folder holding `SQ-INF/manifest.json`), and
`explode(cartridge, folder)` writes one out: sqcart's exploded form, its
rules sqcart's (`SQ-INF/entries.json`, names in NFC, links refused, dot
folders left out unless kept). Neither holds a file whole: pack reads each
at the commit a MiB at a time, and explode writes a chunked entry a
verified chunk at a time. The SDK's tools pack and explode through these
(square's workspaces), never through sqcart directly: sqcart is the format,
the Drive is how the tools move cartridges.

## On Android, and in sqterm

**An app.** Android stops an app that leaves the screen, and may end its
process without another word. squared already says when: the platform calls
`Application::background()` on Android's stop (`sq::App` gives its model a
turn there to save). That is where a Drive's changes are made safe:

```cpp
void background() override {
    drive.sync();    // in the journal, flushed: they survive what follows
}
```

`sync()` is quick (an append and a flush), and what it wrote is replayed
at the next mount if the process is ended before a commit. A `budget` Drive
commits it on its own in the meantime; nothing more is needed. Mount with
`.staging` in the app's private storage (squared's `FileSystem::local`
folder), not shared storage.

**sqterm.** `pkg install sqdrive` (sqterm's `packages/sqdrive`, built from a
tag of this repository with clang 23 against sqterm's sysroot and runtimes
packages). Its journals go in `~/.local/state/sqdrive`, or
`$SQDRIVE_STAGING`.

## Building

```sh
sqmake test                                  # build.lua
CXX=/usr/lib/llvm-23/bin/clang++ sqmake test
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
3. **Writing and the journal** (sqdrive #2).
4. **Background commits and bounds** (sqdrive #3). A 256 MiB file streamed in
   256 KiB writes at `staging_bytes` 24 MiB and `memory_bytes` 4 MiB keeps
   both under their bounds, committing 8 MiB at a time as it goes (the
   design's 2 GiB gate, scaled for CI). A commit reads the chunks it
   writes from the journal (sqcart's `put_range_file`), so while it runs
   it holds none of them in memory.
5. **Hosts**: sqterm's `packages/sqdrive` recipe (`pkg install sqdrive`),
   and Android's stop: squared's `Application::background()` already
   carries it, so an app syncs its Drive there (above); no new hook.

## Licence

MIT. Security issues: see [SECURITY.md](SECURITY.md).
