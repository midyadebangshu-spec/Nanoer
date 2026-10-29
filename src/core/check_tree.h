// fsck of a filesystem with the metadata tree, streaming (docs/M6_PLAN.md
// §10, step 5): the tree is read in key order, keeping a few dozen bytes
// per inode and per directory entry, and lists of users only for packs and
// shared records; repairs change items in the tree. Internal to fsck
// (check.cpp), which loads the superblock, replays the log, walks the
// tree's nodes and writes the repaired tree.
#pragma once

#include <cstdint>
#include <map>

#include "nanoer/check.h"
#include "nanoer/format.h"
#include "nanoer/io.h"
#include "nanoer/metadata.h"

namespace nanoer {

struct TreeCheckInput {
  Store& store;
  const Superblock& sb;
  const FsckOptions& options;
  FsckReport& report;
  Metadata& meta;  // as replayed; with options.repair, repaired in memory
  // Bytes of tree nodes per segment (those the tree holds, and those
  // replaced since the checkpoint), from the node walk.
  const std::map<std::uint64_t, std::uint64_t>& node_bytes;
};

// Checks everything the tree holds. With options.repair, also repairs it
// in `meta`; returns whether anything changed (a checkpoint must follow).
bool check_tree(const TreeCheckInput& input);

}  // namespace nanoer
