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

Milestones M1 (foundation), M2 (working filesystem), M3 (crash safety,
`fsck`) and M4 (garbage collection, dedup, small-file packing, `nanoer
stats`) are complete; M5 (tiering, policies, read-ahead) is next, after its
plan is approved.

- `include/nanoer/`: public headers. `src/core/`: the engine (`fs.cpp` is the
  POSIX layer, `journal.cpp` the log, `metadata.cpp` the operations).
  `src/fuse/`: `nanoerfs` (also `mount.nanoer`). `src/tools/`: `mkfs.nanoer`,
  `nanoer` (also `fsck.nanoer`), `nanoer-fsx`. `src/core/check.cpp`: fsck.
  `src/core/gc.cpp`: garbage collection. `src/core/dedup.cpp`: the dedup
  index. `src/core/stats.cpp`: `nanoer stats`. Packing is in `fs.cpp`'s
  commit.
  `tests/unit/`: GoogleTest (`crash_test.cpp` explores every crash point).
  `tests/integration/`: tests on a real FUSE mount, including `kill -9`.
  `fuzz/`: libFuzzer. `.github/workflows/nightly.yml`: the exhaustive runs.
- `docs/format.md` defines the on-disk format. Any change to the format must
  update it in the same commit.
- `third_party/blake3`: vendored, pinned by `SHA256SUMS`; never edit it.

## Commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # sanitizers
cmake --preset fuzz && cmake --build --preset fuzz                          # Clang only
# The first directory is where libFuzzer writes new inputs: never the seeds.
mkdir -p build/corpus/parse && ./build/fuzz/fuzz/fuzz_parse -max_total_time=60 build/corpus/parse fuzz/seeds/parse
ctest --preset release -L integration      # real FUSE mounts (needs /dev/fuse; root for all tests)
NANOER_EXHAUSTIVE=1 ./build/release/tests/nanoer_unit_tests --gtest_filter='Crash.*'  # every crash point (~15 min)
```

Check that a test filter actually matches before trusting a pass:
parameterized tests are named like `Seeds/FsModel.Random.../0`.

Build with `-DNANOER_WERROR=ON` before committing; CI does.
