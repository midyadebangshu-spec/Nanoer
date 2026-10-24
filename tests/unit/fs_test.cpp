#include "nanoer/fs.h"

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>

#include "nanoer/error.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

Filesystem::Options test_options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;  // commit only when asked, so tests are deterministic
  o.count_lookups = false;
  o.threads = 2;
  o.safe_space = 4 << 20;
  return o;
}

// A filesystem on a FaultStore, with remount and crash helpers.
class TestFs {
 public:
  explicit TestFs(std::uint64_t size = 64 << 20, FormatOptions format = {.segment_shift = 20,
                                                                         .chunk_shift = 14},
                  Filesystem::Options options = test_options())
      : options_(options), store_(std::make_unique<FaultStore>(size)) {
    make_filesystem(*store_, format);
    store_->flush();
    mount();
  }

  Filesystem& operator*() { return *fs_; }
  Filesystem* operator->() { return fs_.get(); }

  void mount() { fs_ = Filesystem::mount(*store_, options_); }
  void remount() {
    fs_.reset();
    mount();
  }
  // Power cut: everything not flushed is lost.
  void crash() {
    fs_->abandon();
    fs_.reset();
    store_ = std::make_unique<FaultStore>(store_->crash_image({}));
    mount();
  }
  FaultStore& store() { return *store_; }
  Filesystem::Options& options() { return options_; }

 private:
  Filesystem::Options options_;
  std::unique_ptr<FaultStore> store_;
  std::unique_ptr<Filesystem> fs_;
};

int error_of(const std::function<void()>& f) {
  try {
    f();
  } catch (const Error& e) {
    return e.to_errno();
  }
  return 0;
}

Bytes pattern(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng() % 32 + 'a');  // compressible
  return b;
}

Bytes read_all(Filesystem& fs, InodeId ino) {
  Bytes out(fs.getattr(ino).size);
  EXPECT_EQ(fs.read(ino, 0, out), out.size());
  return out;
}

InodeId file(Filesystem& fs, InodeId dir, std::string_view name) {
  return fs.mknod(dir, name, S_IFREG | 0644, 0, {}).ino;
}

constexpr InodeId root = kRootInode;

}  // namespace

TEST(Fs, WriteReadAcrossChunksAndRemount) {
  TestFs fs;
  InodeId f = file(*fs, root, "data.bin");
  Bytes data = pattern(300000, 1);  // 16 KiB chunks: about 19 of them
  EXPECT_EQ(fs->write(f, 0, data), data.size());
  EXPECT_EQ(read_all(*fs, f), data);  // before any commit
  fs.remount();
  f = fs->lookup(root, "data.bin").ino;
  EXPECT_EQ(read_all(*fs, f), data);
  EXPECT_EQ(fs->getattr(f).size, data.size());
  // 32 random letters carry 5 bits per byte: at best ~62.5 % after compression.
  FsInfo info = fs->info();
  EXPECT_LT(info.stored_bytes, data.size() * 7 / 10) << "compressible data should shrink";
}

TEST(Fs, UnalignedOverwritesAndHoles) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  Bytes model(100000, 0);
  auto put = [&](std::uint64_t off, const Bytes& d) {
    fs->write(f, off, d);
    if (model.size() < off + d.size()) model.resize(off + d.size(), 0);
    std::copy(d.begin(), d.end(), model.begin() + static_cast<std::ptrdiff_t>(off));
  };
  fs->setattr(f, {.size = 100000});
  put(5, pattern(10, 2));
  put(16380, pattern(10, 3));  // straddles a chunk boundary
  fs->sync();
  put(16383, pattern(2, 4));   // overwrite inside a committed chunk
  put(250000, pattern(100, 5));  // far beyond: a hole in between
  EXPECT_EQ(read_all(*fs, f), model);
  fs.remount();
  EXPECT_EQ(read_all(*fs, f), model);
  // The hole really is a hole.
  EXPECT_EQ(fs->lseek(f, 100000, SEEK_DATA), 245760u);  // chunk 15 starts at 15 * 16 KiB
  EXPECT_EQ(fs->lseek(f, 17000, SEEK_HOLE), 32768u);
}

TEST(Fs, TruncateNeverResurrectsOldBytes) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  fs->write(f, 0, Bytes(50000, 'x'));
  fs->sync();
  fs->setattr(f, {.size = 20000});  // inside chunk 1
  fs->setattr(f, {.size = 50000});  // grow again: 20000.. must be zeros
  Bytes expected(50000, 0);
  std::fill(expected.begin(), expected.begin() + 20000, 'x');
  EXPECT_EQ(read_all(*fs, f), expected);
  fs.remount();
  EXPECT_EQ(read_all(*fs, f), expected);
  fs->setattr(f, {.size = 0});
  EXPECT_EQ(fs->getattr(f).blocks, 0u);
}

TEST(Fs, AllZeroChunksAreStoredAsHoles) {
  TestFs fs;
  InodeId f = file(*fs, root, "zeros");
  fs->write(f, 0, Bytes(1 << 20, 0));
  fs->sync();
  EXPECT_EQ(fs->getattr(f).blocks, 0u);
  EXPECT_EQ(fs->info().stored_bytes, 0u);
  EXPECT_EQ(read_all(*fs, f), Bytes(1 << 20, 0));
}

TEST(Fs, DirectoriesAndErrors) {
  TestFs fs;
  Caller c{1000, 100};
  InodeId a = fs->mkdir(root, "a", 0755, c).ino;
  InodeId b = fs->mkdir(a, "b", 0700, c).ino;
  file(*fs, b, "x");
  EXPECT_EQ(fs->getattr(root).nlink, 3u);
  EXPECT_EQ(fs->getattr(a).nlink, 3u);
  EXPECT_EQ(fs->getattr(b).uid, 1000u);
  EXPECT_EQ(fs->lookup(b, "..").ino, a);

  EXPECT_EQ(error_of([&] { fs->mkdir(root, "a", 0755, c); }), EEXIST);
  EXPECT_EQ(error_of([&] { fs->lookup(root, "nope"); }), ENOENT);
  EXPECT_EQ(error_of([&] { fs->rmdir(root, "a"); }), ENOTEMPTY);
  EXPECT_EQ(error_of([&] { fs->unlink(root, "a"); }), EISDIR);
  EXPECT_EQ(error_of([&] { fs->rmdir(b, "x"); }), ENOTDIR);
  EXPECT_EQ(error_of([&] { fs->lookup(fs->lookup(b, "x").ino, "y"); }), ENOTDIR);
  EXPECT_EQ(error_of([&] { file(*fs, root, std::string(256, 'n')); }), ENAMETOOLONG);
  EXPECT_EQ(error_of([&] { fs->read(a, 0, MutableByteView()); }), EISDIR);

  fs->unlink(b, "x");
  fs->rmdir(a, "b");
  EXPECT_EQ(fs->getattr(a).nlink, 2u);
  fs.remount();
  std::vector<DirEntry> entries = fs->list(root);
  ASSERT_EQ(entries.size(), 3u);  // ".", "..", "a"
  EXPECT_EQ(entries[2].name, "a");
  EXPECT_TRUE(fs->list(a).size() == 2);
}

TEST(Fs, SetgidDirectoriesPassOnTheirGroup) {
  TestFs fs;
  InodeId d = fs->mkdir(root, "shared", 02775, {0, 50}).ino;
  Attr f = fs->mknod(d, "f", S_IFREG | 0644, 0, {1000, 100});
  Attr sub = fs->mkdir(d, "sub", 0755, {1000, 100});
  EXPECT_EQ(f.gid, 50u);
  EXPECT_EQ(sub.gid, 50u);
  EXPECT_TRUE(sub.mode & S_ISGID);
  EXPECT_FALSE(f.mode & S_ISGID);
}

TEST(Fs, HardLinksAndSymlinks) {
  TestFs fs;
  InodeId f = file(*fs, root, "orig");
  fs->write(f, 0, pattern(1000, 7));
  InodeId d = fs->mkdir(root, "d", 0755, {}).ino;
  EXPECT_EQ(fs->link(f, d, "alias").nlink, 2u);
  EXPECT_EQ(error_of([&] { fs->link(d, root, "dirlink"); }), EPERM);
  fs->unlink(root, "orig");
  EXPECT_EQ(fs->getattr(f).nlink, 1u);
  EXPECT_EQ(read_all(*fs, fs->lookup(d, "alias").ino), pattern(1000, 7));

  InodeId s = fs->symlink(root, "sym", "d/alias", {}).ino;
  EXPECT_EQ(fs->readlink(s), "d/alias");
  EXPECT_EQ(fs->getattr(s).size, 7u);
  EXPECT_EQ(error_of([&] { fs->readlink(f); }), EINVAL);
  EXPECT_EQ(error_of([&] { fs->symlink(root, "empty", "", {}); }), ENOENT);
  fs.remount();
  EXPECT_EQ(fs->readlink(fs->lookup(root, "sym").ino), "d/alias");
  EXPECT_EQ(fs->getattr(f).nlink, 1u);
}

TEST(Fs, RenameRules) {
  TestFs fs;
  InodeId a = fs->mkdir(root, "a", 0755, {}).ino;
  InodeId b = fs->mkdir(root, "b", 0755, {}).ino;
  InodeId sub = fs->mkdir(a, "sub", 0755, {}).ino;
  InodeId f1 = file(*fs, root, "f1");
  InodeId f2 = file(*fs, root, "f2");
  fs->write(f2, 0, pattern(100, 1));

  fs->rename(root, "f1", a, "moved", 0);
  EXPECT_EQ(fs->lookup(a, "moved").ino, f1);
  EXPECT_EQ(error_of([&] { fs->lookup(root, "f1"); }), ENOENT);

  fs->rename(root, "f2", a, "moved", 0);  // replaces f1, which is then gone
  EXPECT_EQ(fs->lookup(a, "moved").ino, f2);
  EXPECT_EQ(error_of([&] { fs->getattr(f1); }), ENOENT);

  EXPECT_EQ(error_of([&] { fs->rename(root, "a", sub, "loop", 0); }), EINVAL);
  EXPECT_EQ(error_of([&] { fs->rename(root, "a", a, "self", 0); }), EINVAL);
  EXPECT_EQ(error_of([&] { fs->rename(a, "moved", root, "b", 0); }), EISDIR);
  EXPECT_EQ(error_of([&] { fs->rename(root, "b", a, "moved", 0); }), ENOTDIR);
  fs->mkdir(b, "child", 0755, {});
  EXPECT_EQ(error_of([&] { fs->rename(a, "sub", root, "b", 0); }), ENOTEMPTY);

  // Directory moves update "..", and both parents' link counts.
  fs->rename(a, "sub", b, "sub2", 0);
  EXPECT_EQ(fs->lookup(sub, "..").ino, b);
  EXPECT_EQ(fs->getattr(a).nlink, 2u);
  EXPECT_EQ(fs->getattr(b).nlink, 4u);

  file(*fs, root, "x");
  EXPECT_EQ(error_of([&] { fs->rename(root, "x", a, "moved", RENAME_NOREPLACE); }), EEXIST);
  fs->rename(root, "x", a, "moved", RENAME_EXCHANGE);
  EXPECT_EQ(fs->lookup(root, "x").ino, f2);
  fs->rename(root, "a", b, "child", RENAME_EXCHANGE);  // directories trade places
  EXPECT_EQ(fs->lookup(b, "child").ino, a);
  EXPECT_EQ(fs->lookup(a, "..").ino, b);

  fs.remount();
  EXPECT_EQ(fs->lookup(b, "child").ino, a);
  EXPECT_EQ(fs->lookup(sub, "..").ino, b);
  EXPECT_EQ(read_all(*fs, fs->lookup(root, "x").ino), pattern(100, 1));
}

TEST(Fs, ExtendedAttributes) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  Bytes v = {1, 2, 3};
  fs->setxattr(f, "user.a", v, 0);
  fs->setxattr(f, "security.selinux", as_bytes("system_u:object_r:etc_t:s0"), 0);
  EXPECT_EQ(fs->getxattr(f, "user.a"), v);
  EXPECT_EQ(error_of([&] { fs->setxattr(f, "user.a", v, XATTR_CREATE); }), EEXIST);
  EXPECT_EQ(error_of([&] { fs->setxattr(f, "user.b", v, XATTR_REPLACE); }), ENODATA);
  EXPECT_EQ(error_of([&] { fs->getxattr(f, "user.b"); }), ENODATA);
  EXPECT_EQ(error_of([&] { fs->setxattr(f, "user.big", Bytes(65537), 0); }), E2BIG);
  fs.remount();
  std::vector<std::string> names = fs->listxattr(f);
  EXPECT_EQ(names, (std::vector<std::string>{"security.selinux", "user.a"}));
  fs->removexattr(f, "user.a");
  EXPECT_EQ(error_of([&] { fs->removexattr(f, "user.a"); }), ENODATA);
}

TEST(Fs, AttributesAndTimes) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  Attr before = fs->getattr(f);
  fs->setattr(f, {.mode = 04755, .uid = 5, .gid = 6});
  fs->setattr(f, {.atime = SetAttrRequest::Time::set, .mtime = SetAttrRequest::Time::set,
                  .atime_ns = 1'000'000'001, .mtime_ns = -2'000'000'000});
  Attr a = fs->getattr(f);
  EXPECT_EQ(a.mode, static_cast<std::uint32_t>(S_IFREG | 04755));
  EXPECT_EQ(a.uid, 5u);
  EXPECT_EQ(a.gid, 6u);
  EXPECT_EQ(a.atime_ns, 1'000'000'001);
  EXPECT_EQ(a.mtime_ns, -2'000'000'000);
  EXPECT_GE(a.ctime_ns, before.ctime_ns);
  fs.remount();
  EXPECT_EQ(fs->getattr(f).mtime_ns, -2'000'000'000);
  EXPECT_EQ(fs->getattr(f).atime_ns, 1'000'000'001);
}

TEST(Fs, SpecialFiles) {
  TestFs fs;
  Attr fifo = fs->mknod(root, "fifo", S_IFIFO | 0600, 0, {});
  Attr dev = fs->mknod(root, "null", S_IFCHR | 0666, makedev(1, 3), {});
  fs.remount();
  EXPECT_TRUE(S_ISFIFO(fs->getattr(fifo.ino).mode));
  EXPECT_EQ(fs->getattr(dev.ino).rdev, makedev(1, 3));
  EXPECT_EQ(error_of([&] { fs->write(fifo.ino, 0, Bytes(1)); }), EINVAL);
}

TEST(Fs, PunchHole) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  Bytes data = pattern(100000, 3);
  fs->write(f, 0, data);
  fs->sync();
  fs->fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 10000, 50000);
  std::fill(data.begin() + 10000, data.begin() + 60000, 0);
  EXPECT_EQ(read_all(*fs, f), data);
  EXPECT_EQ(fs->getattr(f).size, 100000u);
  EXPECT_EQ(fs->lseek(f, 20000, SEEK_DATA), 49152u);  // chunks 1 and 2 were dropped
  fs.remount();
  EXPECT_EQ(read_all(*fs, f), data);
  EXPECT_EQ(error_of([&] { fs->fallocate(f, FALLOC_FL_PUNCH_HOLE, 0, 10); }), EOPNOTSUPP);
  fs->fallocate(f, 0, 0, 200000);  // plain allocate extends the size
  EXPECT_EQ(fs->getattr(f).size, 200000u);
  EXPECT_EQ(error_of([&] { fs->lseek(f, 300000, SEEK_DATA); }), ENXIO);
}

TEST(Fs, CrashKeepsExactlyWhatWasSynced) {
  TestFs fs;
  InodeId kept = file(*fs, root, "kept");
  fs->write(kept, 0, pattern(70000, 1));
  fs->sync();
  InodeId lost = file(*fs, root, "lost");
  fs->write(lost, 0, pattern(1000, 2));
  fs->write(kept, 0, pattern(10, 3));  // unsynced overwrite
  fs.crash();
  EXPECT_EQ(read_all(*fs, fs->lookup(root, "kept").ino), pattern(70000, 1));
  EXPECT_EQ(error_of([&] { fs->lookup(root, "lost"); }), ENOENT);
  EXPECT_GT(fs->info().replayed_transactions, 0u) << "recovery should replay the log";
  // It keeps working afterwards.
  InodeId again = file(*fs, root, "lost");
  EXPECT_NE(again, 0u);
  fs.remount();
  EXPECT_EQ(fs->lookup(root, "lost").ino, again);
}

TEST(Fs, DeletedFilesStayReadableWhileOpen) {
  Filesystem::Options o = test_options();
  o.count_lookups = true;  // as under FUSE
  TestFs fs(64 << 20, {.segment_shift = 20, .chunk_shift = 14}, o);
  InodeId f = fs->mknod(root, "tmp", S_IFREG | 0600, 0, {}).ino;
  fs->open(f);
  fs->write(f, 0, pattern(5000, 9));
  fs->unlink(root, "tmp");
  EXPECT_EQ(fs->getattr(f).nlink, 0u);
  EXPECT_EQ(read_all(*fs, f), pattern(5000, 9));
  fs->sync();
  // Power cut while it is still open: the orphan is cleaned up on mount.
  fs.crash();
  EXPECT_EQ(error_of([&] { fs->getattr(f); }), ENOENT);

  InodeId g = fs->mknod(root, "tmp2", S_IFREG | 0600, 0, {}).ino;
  fs->open(g);
  fs->unlink(root, "tmp2");
  fs->forget(g, 1);
  EXPECT_NO_THROW(fs->getattr(g));  // still open
  fs->release(g);
  EXPECT_EQ(error_of([&] { fs->getattr(g); }), ENOENT);
}

TEST(Fs, ReadOnlyMount) {
  TestFs fs;
  InodeId f = file(*fs, root, "f");
  fs->write(f, 0, pattern(100, 1));
  fs.options().read_only = true;
  fs.remount();
  EXPECT_TRUE(fs->read_only());
  EXPECT_EQ(read_all(*fs, f), pattern(100, 1));
  EXPECT_EQ(error_of([&] { file(*fs, root, "g"); }), EROFS);
  EXPECT_EQ(error_of([&] { fs->write(f, 0, Bytes(1)); }), EROFS);
  EXPECT_EQ(fs->statfs().available_blocks, 0u);
}

TEST(Fs, FillingUpGivesEnospcAndDeletingFreesSpace) {
  // 12 segments of 1 MiB; incompressible data so it cannot shrink.
  TestFs fs(min_store_size(20) + (8 << 20), {.segment_shift = 20, .chunk_shift = 14});
  std::mt19937_64 rng(1);
  Bytes noise(64 << 10);
  int written = 0;
  int err = 0;
  for (; written < 1000 && !err; ++written) {
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng());
    err = error_of([&] { fs->write(file(*fs, root, "f" + std::to_string(written)), 0, noise); });
  }
  EXPECT_EQ(err, ENOSPC);
  EXPECT_GT(written, 50);
  // The filesystem is still healthy, and space comes back after deletes.
  for (int i = 0; i < written - 1; ++i) fs->unlink(root, "f" + std::to_string(i));
  fs->sync();
  EXPECT_EQ(error_of([&] { fs->write(file(*fs, root, "after"), 0, noise); }), 0);
  fs.remount();
  EXPECT_EQ(read_all(*fs, fs->lookup(root, "after").ino), noise);
}

TEST(Fs, CheckpointsAndReclaimDuringLongUse) {
  Filesystem::Options o = test_options();
  o.checkpoint_bytes = 2 << 20;  // checkpoint often
  TestFs fs(32 << 20, {.segment_shift = 20, .chunk_shift = 14}, o);
  InodeId f = file(*fs, root, "rewritten");
  // Rewrite one file many times: far more data than the store holds, so
  // only reclaiming dead segments keeps it going.
  for (int round = 0; round < 60; ++round) {
    Bytes data = pattern(1 << 20, static_cast<std::uint64_t>(round));
    for (auto& b : data) b ^= static_cast<std::uint8_t>(round * 37);
    fs->write(f, 0, data);
    fs->sync();
  }
  Bytes last = pattern(1 << 20, 59);
  for (auto& b : last) b ^= static_cast<std::uint8_t>(59 * 37);
  EXPECT_EQ(read_all(*fs, f), last);
  fs.crash();  // recovery from a checkpoint plus the log after it
  EXPECT_EQ(read_all(*fs, fs->lookup(root, "rewritten").ino), last);
}

TEST(Fs, StatfsIsSane) {
  TestFs fs;
  FsStats s = fs->statfs();
  EXPECT_EQ(s.block_size, 4096u);
  EXPECT_GT(s.total_blocks, s.free_blocks);
  EXPECT_GE(s.free_blocks, s.available_blocks);
  EXPECT_EQ(s.files, 1u);
  EXPECT_EQ(s.name_max, 255u);
}

TEST(Fs, ConcurrentReadersAndWriters) {
  TestFs fs;
  std::vector<InodeId> files;
  for (int i = 0; i < 8; ++i) files.push_back(file(*fs, root, "f" + std::to_string(i)));
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      Bytes data = pattern(40000, static_cast<std::uint64_t>(t));
      for (int round = 0; round < 20; ++round) {
        fs->write(files[t], 0, data);
        Bytes back(data.size());
        fs->read(files[t], 0, back);
        if (back != data) ++failures;
        if (round % 5 == 0) fs->sync();
        fs->getattr(files[(t + 1) % 8]);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures, 0);
  fs.remount();
  for (int t = 0; t < 8; ++t) EXPECT_EQ(read_all(*fs, files[t]), pattern(40000, t));
}
