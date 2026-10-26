// Damaged filesystem images: mount, read everything, fsck --repair, fsck.
// Nothing may crash or hang, and a repaired filesystem must check clean.
//
// The input is a list of damage instructions (4-byte offset, 1-byte XOR
// mask) applied to a small, valid image built once at start-up.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>

#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

namespace {

using namespace nanoer;

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  o.start_threads = false;
  o.safe_space = 1 << 20;
  return o;
}

struct Base {
  Bytes image;
  std::uint64_t used = 0;  // damage lands in [0, used): where the data is
};

const Base& base() {
  static const Base b = [] {
    MemStore store(min_store_size(20) + (1 << 20));  // 5 segments of 1 MiB
    make_filesystem(store, {.segment_shift = 20, .chunk_shift = 12});
    {
      auto fs = Filesystem::mount(store, options());
      InodeId d = fs->mkdir(kRootInode, "dir", 0755, {}).ino;
      for (int i = 0; i < 6; ++i) {
        InodeId f = fs->mknod(i % 2 ? d : kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
        Bytes data(3000 + static_cast<std::size_t>(i) * 2500);
        for (std::size_t j = 0; j < data.size(); ++j) data[j] = static_cast<std::uint8_t>(j * (i + 3));
        fs->write(f, 0, data);
        fs->setxattr(f, "user.i", as_bytes(std::to_string(i)), 0);
        if (i == 2) fs->sync();
      }
      fs->symlink(d, "link", "../f0", {});
      fs->link(fs->lookup(kRootInode, "f0").ino, d, "hard");
      // Small files written together: a pack.
      InodeId small = fs->mkdir(kRootInode, "small", 0755, {}).ino;
      for (int i = 0; i < 6; ++i) {
        Bytes data(150 + static_cast<std::size_t>(i) * 300, static_cast<std::uint8_t>('a' + i));
        fs->write(fs->mknod(small, "s" + std::to_string(i), S_IFREG | 0644, 0, {}).ino, 0, data);
      }
      // A copy: its chunks are shared with f5's (dedup).
      InodeId f5 = fs->lookup(d, "f5").ino;
      Bytes copy(fs->getattr(f5).size);
      fs->read(f5, 0, copy);
      fs->write(fs->mknod(kRootInode, "copy", S_IFREG | 0644, 0, {}).ino, 0, copy);
    }  // unmount: checkpoint
    {
      // A little log after the checkpoint, as after a crash.
      auto fs = Filesystem::mount(store, options());
      fs->rename(kRootInode, "f2", fs->lookup(kRootInode, "dir").ino, "moved", 0);
      fs->unlink(kRootInode, "f4");
      fs->sync();
      fs->abandon();
    }
    Base out{store.image(), 0};
    for (std::uint64_t i = out.image.size(); i > 0; --i) {
      if (out.image[i - 1]) {
        out.used = i;
        break;
      }
    }
    return out;
  }();
  return b;
}

void read_everything(Filesystem& fs, InodeId dir, int depth) {
  if (depth > 8) return;
  for (const DirEntry& e : fs.list(dir)) {
    if (e.name == "." || e.name == "..") continue;
    try {
      Attr a = fs.getattr(e.ino);
      if (S_ISDIR(a.mode)) {
        read_everything(fs, e.ino, depth + 1);
      } else if (S_ISREG(a.mode) && a.size < (1 << 22)) {
        Bytes buf(a.size);
        fs.read(e.ino, 0, buf);
      } else if (S_ISLNK(a.mode)) {
        fs.readlink(e.ino);
      }
      for (const std::string& x : fs.listxattr(e.ino)) fs.getxattr(e.ino, x);
    } catch (const Error& err) {
      if (err.code() != Errc::corrupt && err.code() != Errc::posix) throw;
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const Base& b = base();
  Bytes image = b.image;
  for (std::size_t i = 0; i + 5 <= size && i < 5 * 64; i += 5) {
    std::uint64_t at = nanoer::load_le<std::uint32_t>(data + i) % b.used;
    image[at] ^= data[i + 4] ? data[i + 4] : 1;
  }

  // Mounting (read-write: it writes, too) and reading everything.
  {
    MemStore store(image);
    try {
      auto fs = Filesystem::mount(store, options());
      read_everything(*fs, kRootInode, 0);
      fs->abandon();
    } catch (const Error& e) {
      if (e.code() != Errc::corrupt && e.code() != Errc::unsupported) __builtin_trap();
    }
  }

  // Repairing, then checking the repair.
  MemStore store(image);
  try {
    FsckReport repaired = fsck(store, {.repair = true, .deep = true});
    // Some damage is beyond repair, and fsck says so; then there is nothing
    // further to check. A repair that claims success must be real, though.
    if (!repaired.errors.empty()) return 0;
  } catch (const Error& e) {
    // fsck may give up on a filesystem it cannot find; it must not crash.
    if (e.code() != Errc::corrupt && e.code() != Errc::unsupported) __builtin_trap();
    return 0;
  }
  FsckReport after = fsck(store, {.repair = false, .deep = true});
  if (!after.errors.empty()) {  // a repair must leave it clean
    for (const std::string& e : after.errors) std::fprintf(stderr, "after repair: %s\n", e.c_str());
    __builtin_trap();
  }
  auto fs = Filesystem::mount(store, options());
  read_everything(*fs, kRootInode, 0);
  fs->abandon();
  return 0;
}
