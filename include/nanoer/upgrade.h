// nanoer upgrade (docs/M6_PLAN.md §4): a filesystem made by M2-M5 keeps all
// its metadata in full checkpoints; this writes it as a metadata tree into
// free segments and switches over with one superblock write, the same
// atomic step as every checkpoint. A crash before that write leaves the old
// filesystem as it was; after it, the new one is complete.
#pragma once

#include <cstdint>

#include "nanoer/io.h"

namespace nanoer {

struct UpgradeReport {
  bool already = false;  // the filesystem has the metadata tree already: nothing done
  std::uint64_t inodes = 0;
  std::uint64_t chunks = 0;
  // Records with one user that their header does not name (left by dedup
  // when the record's first owner went away): they keep a list of users,
  // as GC needs one to find theirs.
  std::uint64_t named_users = 0;
  std::uint64_t node_bytes = 0;      // the tree as written
  std::uint64_t old_checkpoint = 0;  // bytes of the old full checkpoint
};

// Upgrades the filesystem on `store` (not mounted). Throws Error, having
// changed nothing the old filesystem needs, if it is damaged (run fsck
// first), or if there is no room for the tree (no_space, with the amount).
UpgradeReport upgrade(Store& store);

}  // namespace nanoer
