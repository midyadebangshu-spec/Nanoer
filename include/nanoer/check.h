// nanoer fsck: check a filesystem offline, and repair what is safe to repair.
//
// Checking never writes. Repairing works like a mount: it loads the
// metadata, fixes it in memory, then writes a new checkpoint and points the
// superblock at it; nothing is changed in place. Data that cannot be read
// back intact is never guessed at: it is reported as lost, by file and byte
// range. See docs/M3_PLAN.md §4.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nanoer {

class Store;

struct FsckOptions {
  bool repair = false;
  bool deep = false;  // also read and verify every data chunk
};

struct FsckReport {
  std::vector<std::string> errors;  // problems that remain
  std::vector<std::string> fixed;   // problems found and repaired
  std::vector<std::string> notes;   // worth knowing; nothing to fix
  std::vector<std::string> lost;    // what could not be kept, e.g. "/a/b: bytes 0-16383"

  std::uint64_t files = 0;
  std::uint64_t directories = 0;
  std::uint64_t chunks = 0;
  std::uint64_t chunks_verified = 0;  // with deep

  // e2fsck conventions: 0 clean, 1 errors fixed, 4 errors left.
  int exit_code() const;
};

// Checks (and with options.repair, repairs) the filesystem on `store`.
// Throws Error only when it cannot run at all (an I/O error, a store that
// holds no Nanoer filesystem it can find).
FsckReport fsck(Store& store, const FsckOptions& options);

}  // namespace nanoer
