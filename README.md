# Nanoer

**Transparent, OS-level compression for Linux.** Store more on the same disk:
with identical contents, a system that needs 800 GB without Nanoer should need
about 500 GB with it. Programs, games and the OS keep using normal files and
never notice the data is compressed.

Nanoer stores data as independently compressed chunks and decompresses only
what is read (**live extraction**), within a bounded memory budget (the **safe
space**).

## Status

Planning **stage 1**: a C++20 engine and read-write FUSE filesystem
(`nanoerfs`). The plan is in [`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md) and
is waiting for approval. The earlier Python prototype has been removed; it
remains in git history.

| Stage | Scope |
|---|---|
| 1 | C++ engine + read-write FUSE filesystem for folders (`/home`, `/opt`, games) |
| 2 | Whole OS: compressed system image + writable layer, booted from initramfs |
| 3 | Kernel driver (C or Rust) for full speed |

## What compresses

Measured on Linux system files: about **3 : 1** (stored at 30–36 % of the
original). Already-encoded media (video, music, JPEG/PNG, most game assets)
does not compress; Nanoer stores it raw, so it costs nothing extra but saves
nothing either.

## Documentation

- [`KNOWLEDGE.md`](KNOWLEDGE.md): every technology, algorithm and design
  decision, with the reasoning and measurements.
- [`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md): the current plan.

## Licence

[Apache License 2.0](LICENSE).
