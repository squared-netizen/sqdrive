# sqdrive

The Virtual Cartridge Drive: the big-file interface for squared apps. Mount
a 3.0 cartridge and read and write files of any size by range; staged
changes survive a crash and are committed through `sq.cart.write`.

- `sq.drive`: the C++23 module apps link.
- `sqdrive`: the command-line tool (`cat`, `write`, `ls`, `stat`, `sync`,
  `commit`, `recover`, `bench`).

In development; nothing to build yet.
