// Model-based testing: random operations are applied both to the filesystem
// and to a deliberately simple in-memory model of what POSIX says should
// happen. The two are compared regularly, and after every remount and
// simulated power cut (where the model rolls back to the last sync).
#include <map>
#include <random>
#include <set>

#include <gtest/gtest.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <sys/stat.h>

#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

struct Node {
  std::uint32_t type;  // S_IFREG, S_IFDIR or S_IFLNK
  Bytes data;
  std::string target;
  std::map<std::string, InodeId> entries;
  std::map<std::string, Bytes> xattrs;
  InodeId parent = 0;
};

struct Model {
  std::map<InodeId, Node> nodes;

  Model() { nodes[kRootInode] = Node{S_IFDIR, {}, {}, {}, {}, kRootInode}; }

  int links(InodeId ino) const {
    int n = 0;
    for (const auto& [id, node] : nodes) {
      for (const auto& [name, child] : node.entries) n += child == ino;
    }
    return n;
  }
  bool is_descendant(InodeId dir, InodeId of) const {  // dir is `of` or inside it
    for (InodeId at = dir;; at = nodes.at(at).parent) {
      if (at == of) return true;
      if (at == kRootInode) return false;
    }
  }
  void drop_if_unlinked(InodeId ino) {
    if (links(ino) == 0) nodes.erase(ino);
  }
};

class ModelTest {
 public:
  explicit ModelTest(std::uint64_t seed) : rng_(seed) {
    // Small segments: without M4's copying garbage collection, only segments
    // whose data is entirely dead are reclaimed, and small ones get there
    // sooner. The test must stay within capacity, not measure it.
    store_ = std::make_unique<FaultStore>(96 << 20);
    make_filesystem(*store_, {.segment_shift = 20, .chunk_shift = 14});
    store_->flush();
    options_.commit_interval_s = 0;
    options_.count_lookups = false;
    options_.threads = 2;
    options_.safe_space = 2 << 20;
    options_.dirty_limit = 1ull << 40;   // commits only on sync (and admission)
    options_.checkpoint_bytes = 8 << 20;  // exercise checkpoints and reclaim
    fs_ = Filesystem::mount(*store_, options_);
  }

  void run(int steps) {
    for (int step = 0; step < steps; ++step) {
      int choice = pick(100);
      if (choice < 14) create(S_IFREG);
      else if (choice < 34) write();
      else if (choice < 40) truncate();
      else if (choice < 46) create(S_IFDIR);
      else if (choice < 54) unlink();
      else if (choice < 58) rmdir();
      else if (choice < 70) rename();
      else if (choice < 74) link();
      else if (choice < 77) create(S_IFLNK);
      else if (choice < 82) xattr();
      else if (choice < 85) punch();
      else if (choice < 93) read_check();
      else if (choice < 97) sync();
      else if (choice < 99) remount();
      else crash();
      if (step % 50 == 0) compare();
    }
    compare();
    remount();
  }

 private:
  int pick(int n) { return static_cast<int>(rng_() % static_cast<std::uint64_t>(n)); }
  std::string name() { return "n" + std::to_string(pick(12)); }

  template <class Pred>
  InodeId pick_node(Pred pred) {
    std::vector<InodeId> ids;
    for (const auto& [id, node] : model_.nodes) {
      if (pred(node)) ids.push_back(id);
    }
    return ids.empty() ? 0 : ids[static_cast<std::size_t>(pick(static_cast<int>(ids.size())))];
  }
  InodeId pick_dir() { return pick_node([](const Node& n) { return n.type == S_IFDIR; }); }
  InodeId pick_file() { return pick_node([](const Node& n) { return n.type == S_IFREG; }); }
  // A random (directory, name) entry.
  std::optional<std::pair<InodeId, std::string>> pick_entry() {
    std::vector<std::pair<InodeId, std::string>> all;
    for (const auto& [id, node] : model_.nodes) {
      for (const auto& [n, child] : node.entries) all.emplace_back(id, n);
    }
    if (all.empty()) return std::nullopt;
    return all[static_cast<std::size_t>(pick(static_cast<int>(all.size())))];
  }

  int expect_error(const std::function<void()>& f) {
    try {
      f();
    } catch (const Error& e) {
      return e.to_errno();
    }
    return 0;
  }

  Bytes random_data(std::size_t n) {
    Bytes b(n);
    int style = pick(3);
    for (auto& x : b) {
      x = style == 0 ? static_cast<std::uint8_t>(rng_()) : style == 1 ? 0 : static_cast<std::uint8_t>('a' + rng_() % 4);
    }
    return b;
  }

  void create(std::uint32_t type) {
    InodeId dir = pick_dir();
    std::string n = name();
    bool exists = model_.nodes[dir].entries.contains(n);
    std::string target = "target-" + std::to_string(pick(1000));
    InodeId ino = 0;
    int err = expect_error([&] {
      if (type == S_IFREG) ino = fs_->mknod(dir, n, S_IFREG | 0644, 0, {}).ino;
      if (type == S_IFDIR) ino = fs_->mkdir(dir, n, 0755, {}).ino;
      if (type == S_IFLNK) ino = fs_->symlink(dir, n, target, {}).ino;
    });
    if (exists) {
      ASSERT_EQ(err, EEXIST);
      return;
    }
    if (err == ENOSPC) return;  // refused up front: nothing may have changed
    ASSERT_EQ(err, 0);
    ASSERT_FALSE(model_.nodes.contains(ino)) << "inode number reused while in use";
    model_.nodes[ino] = Node{type, {}, type == S_IFLNK ? target : "", {}, {}, dir};
    model_.nodes[dir].entries[n] = ino;
  }

  void write() {
    InodeId f = pick_file();
    if (!f) return;
    std::uint64_t off = rng_() % 200000;
    Bytes data = random_data(1 + rng_() % 40000);
    if (int err = expect_error([&] { fs_->write(f, off, data); })) {
      ASSERT_EQ(err, ENOSPC);
      return;
    }
    Bytes& d = model_.nodes[f].data;
    if (d.size() < off + data.size()) d.resize(off + data.size(), 0);
    std::copy(data.begin(), data.end(), d.begin() + static_cast<std::ptrdiff_t>(off));
  }

  void truncate() {
    InodeId f = pick_file();
    if (!f) return;
    std::uint64_t size = rng_() % 250000;
    if (int err = expect_error([&] { fs_->setattr(f, {.size = size}); })) {
      ASSERT_EQ(err, ENOSPC);
      return;
    }
    model_.nodes[f].data.resize(size, 0);
  }

  void punch() {
    InodeId f = pick_file();
    if (!f) return;
    std::uint64_t off = rng_() % 300000;
    std::uint64_t len = 1 + rng_() % 200000;
    if (int err = expect_error([&] { fs_->fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len); })) {
      ASSERT_EQ(err, ENOSPC);
      return;
    }
    Bytes& d = model_.nodes[f].data;
    for (std::uint64_t i = off; i < std::min<std::uint64_t>(d.size(), off + len); ++i) d[i] = 0;
  }

  void read_check() {
    InodeId f = pick_file();
    if (!f) return;
    const Bytes& d = model_.nodes[f].data;
    std::uint64_t off = rng_() % (d.size() + 1000);
    Bytes out(rng_() % 200000);
    std::size_t n = fs_->read(f, off, out);
    std::size_t expected = off >= d.size() ? 0 : std::min<std::size_t>(out.size(), d.size() - off);
    ASSERT_EQ(n, expected);
    ASSERT_TRUE(std::equal(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n),
                           d.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(off, d.size()))));
  }

  void unlink() {
    auto e = pick_entry();
    if (!e) return;
    auto [dir, n] = *e;
    InodeId target = model_.nodes[dir].entries[n];
    int err = expect_error([&] { fs_->unlink(dir, n); });
    if (err == ENOSPC) return;
    if (model_.nodes[target].type == S_IFDIR) {
      ASSERT_EQ(err, EISDIR);
      return;
    }
    ASSERT_EQ(err, 0);
    model_.nodes[dir].entries.erase(n);
    model_.drop_if_unlinked(target);
  }

  void rmdir() {
    auto e = pick_entry();
    if (!e) return;
    auto [dir, n] = *e;
    InodeId target = model_.nodes[dir].entries[n];
    int err = expect_error([&] { fs_->rmdir(dir, n); });
    if (err == ENOSPC) return;
    const Node& t = model_.nodes[target];
    if (t.type != S_IFDIR) {
      ASSERT_EQ(err, ENOTDIR);
      return;
    }
    if (!t.entries.empty()) {
      ASSERT_EQ(err, ENOTEMPTY);
      return;
    }
    ASSERT_EQ(err, 0);
    model_.nodes[dir].entries.erase(n);
    model_.nodes.erase(target);
  }

  void rename() {
    auto e = pick_entry();
    if (!e) return;
    auto [dir, n] = *e;
    InodeId to_dir = pick_dir();
    std::string to = name();
    InodeId src = model_.nodes[dir].entries[n];
    auto& dst_entries = model_.nodes[to_dir].entries;
    InodeId dst = dst_entries.contains(to) ? dst_entries[to] : 0;
    bool src_dir = model_.nodes[src].type == S_IFDIR;

    int expected = 0;
    if (dir == to_dir && n == to) expected = 0;
    else if (src_dir && model_.is_descendant(to_dir, src)) expected = EINVAL;
    else if (dst == src) expected = 0;
    else if (dst && src_dir && model_.nodes[dst].type != S_IFDIR) expected = ENOTDIR;
    else if (dst && !src_dir && model_.nodes[dst].type == S_IFDIR) expected = EISDIR;
    else if (dst && model_.nodes[dst].type == S_IFDIR && !model_.nodes[dst].entries.empty()) expected = ENOTEMPTY;

    int err = expect_error([&] { fs_->rename(dir, n, to_dir, to, 0); });
    if (err == ENOSPC && expected == 0) return;
    ASSERT_EQ(err, expected) << "rename " << dir << "/" << n << " -> " << to_dir << "/" << to;
    if (err || (dir == to_dir && n == to) || dst == src) return;
    model_.nodes[to_dir].entries[to] = src;
    model_.nodes[dir].entries.erase(n);
    if (src_dir) model_.nodes[src].parent = to_dir;
    if (dst) {
      if (model_.nodes[dst].type == S_IFDIR) model_.nodes.erase(dst);
      else model_.drop_if_unlinked(dst);
    }
  }

  void link() {
    InodeId f = pick_file();
    if (!f) return;
    InodeId dir = pick_dir();
    std::string n = name();
    int err = expect_error([&] { fs_->link(f, dir, n); });
    if (err == ENOSPC) return;
    if (model_.nodes[dir].entries.contains(n)) {
      ASSERT_EQ(err, EEXIST);
      return;
    }
    ASSERT_EQ(err, 0);
    model_.nodes[dir].entries[n] = f;
  }

  void xattr() {
    InodeId ino = pick_node([](const Node&) { return true; });
    std::string n = "user.x" + std::to_string(pick(3));
    auto& xattrs = model_.nodes[ino].xattrs;
    if (pick(3) == 0) {
      int err = expect_error([&] { fs_->removexattr(ino, n); });
      if (err == ENOSPC) return;
      ASSERT_EQ(err, xattrs.contains(n) ? 0 : ENODATA);
      xattrs.erase(n);
    } else {
      Bytes v = random_data(rng_() % 300);
      if (int err = expect_error([&] { fs_->setxattr(ino, n, v, 0); })) {
        ASSERT_EQ(err, ENOSPC);
        return;
      }
      xattrs[n] = v;
    }
  }

  void sync() {
    fs_->sync();
    synced_ = model_;
  }

  void remount() {
    fs_.reset();
    fs_ = Filesystem::mount(*store_, options_);
    synced_ = model_;
    compare();
  }

  void crash() {
    fs_->abandon();
    fs_.reset();
    store_ = std::make_unique<FaultStore>(store_->crash_image({}));
    fs_ = Filesystem::mount(*store_, options_);
    model_ = synced_;
    compare();
  }

  void compare() { compare_dir(kRootInode); }

  void compare_dir(InodeId dir) {
    const Node& m = model_.nodes.at(dir);
    std::vector<DirEntry> entries = fs_->list(dir);
    ASSERT_GE(entries.size(), 2u);
    ASSERT_EQ(entries[1].ino, m.parent) << "'..' of " << dir;
    ASSERT_EQ(entries.size() - 2, m.entries.size()) << "entry count of " << dir;
    std::uint32_t subdirs = 0;
    for (std::size_t i = 2; i < entries.size(); ++i) {
      const DirEntry& e = entries[i];
      auto it = m.entries.find(e.name);
      ASSERT_NE(it, m.entries.end()) << "unexpected entry " << e.name;
      ASSERT_EQ(it->second, e.ino) << e.name;
      const Node& child = model_.nodes.at(e.ino);
      Attr a = fs_->getattr(e.ino);
      ASSERT_EQ(a.mode & S_IFMT, child.type) << e.name;
      ASSERT_EQ(fs_->listxattr(e.ino).size(), child.xattrs.size()) << e.name;
      for (const auto& [n, v] : child.xattrs) ASSERT_EQ(fs_->getxattr(e.ino, n), v);
      if (child.type == S_IFDIR) {
        ++subdirs;
        compare_dir(e.ino);
      } else if (child.type == S_IFREG) {
        ASSERT_EQ(a.nlink, static_cast<std::uint32_t>(model_.links(e.ino))) << e.name;
        ASSERT_EQ(a.size, child.data.size()) << e.name;
        Bytes data(child.data.size());
        fs_->read(e.ino, 0, data);
        ASSERT_TRUE(data == child.data) << "contents of " << e.name << " (inode " << e.ino << ")";
      } else {
        ASSERT_EQ(fs_->readlink(e.ino), child.target);
      }
    }
    ASSERT_EQ(fs_->getattr(dir).nlink, 2 + subdirs) << "link count of directory " << dir;
  }

  std::mt19937_64 rng_;
  std::unique_ptr<FaultStore> store_;
  Filesystem::Options options_;
  std::unique_ptr<Filesystem> fs_;
  Model model_;
  Model synced_;
};

}  // namespace

class FsModel : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(FsModel, RandomOperationsMatchTheModel) {
  ModelTest test(GetParam());
  test.run(1500);
}

INSTANTIATE_TEST_SUITE_P(Seeds, FsModel, ::testing::Values(1, 2, 3, 4));
