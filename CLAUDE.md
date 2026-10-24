# CLAUDE.md

Guidance for Claude when working in this repository.

## Working rules

- **Plan first for multistep work.** Before starting any task that takes more
  than one step (new features, refactors, changes across several files, new
  tooling), write a short plan covering the approach, the files involved, the
  design choices and the open questions. Share it, discuss it, and wait for
  explicit approval before writing code. If the plan has to change partway
  through, stop and get the new plan approved too.
- Single-step requests (answering a question, a one-line fix, a typo) can go
  ahead without a plan.
- **Keep `KNOWLEDGE.md` current.** Whenever a technology, algorithm, design
  decision, measurement or rejected alternative is added or changed, record
  it in `KNOWLEDGE.md` (including the reason) in the same commit. New
  decisions also go in its decision log.

## Project overview

Nanoer is transparent, OS-level compression for Linux: data is stored as
independently compressed chunks and decompressed on demand (**live
extraction**) within a bounded memory budget (the **safe space**). The goal is
that a system needing 800 GB without Nanoer needs about 500 GB with it.

- Stage 1 (current): C++20 engine + read-write FUSE filesystem `nanoerfs`.
  Plan: `docs/STAGE1_PLAN.md`.
- Stage 2: whole OS on Nanoer. Stage 3: kernel driver in C or Rust.
- `KNOWLEDGE.md`: all technical background, reasoning, measurements and the
  decision log.
- Licence: Apache-2.0. A future kernel module will be Dual MIT/GPL.

Milestone M1 (foundation) is complete; M2 (the working filesystem) is next.

- `include/nanoer/`: public headers. `src/core/`: the engine. `src/tools/`:
  `mkfs.nanoer`, `nanoer`. `tests/unit/`: GoogleTest. `fuzz/`: libFuzzer.
- `docs/format.md` defines the on-disk format. Any change to the format must
  update it in the same commit.
- `third_party/blake3`: vendored, pinned by `SHA256SUMS`; never edit it.

## Commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # sanitizers
cmake --preset fuzz && cmake --build --preset fuzz                          # Clang only
./build/fuzz/fuzz/fuzz_parse -max_total_time=60 fuzz/seeds/parse
```

Build with `-DNANOER_WERROR=ON` before committing; CI does.
