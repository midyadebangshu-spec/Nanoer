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

No code exists yet; the previous Python prototype was removed and remains in
git history.
