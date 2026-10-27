// Crash-point exploration (in the style of CrashMonkey and ALICE).
//
// A workload runs once on a store that records every write and flush.
// Then, for every write, the test builds the images a power cut right after
// it could leave behind: unflushed writes all lost, all kept, each one lost
// on its own (the disk reordered), the last one torn at a sector boundary,
// random mixtures. Every image must mount, and must hold exactly the state
// of a real commit, never losing anything that a sync had made durable.
//
// Set NANOER_EXHAUSTIVE=1 to try every crash point and every variant (the
// nightly CI run); by default a fixed sample keeps the test fast.

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

bool exhaustive() { return std::getenv("NANOER_EXHAUSTIVE") != nullptr; }

// Records every write and flush; can show the store as any crash would leave
// it. Writes are recorded per 4 KiB block: a disk may persist the blocks of
// one large write in any order, or only some of them.
class RecordingStore final : public Store {
 public:
  struct Event {
    bool flush;
    std::uint64_t offset;
    Bytes data;
  };

  explicit RecordingStore(Bytes initial) : initial_(initial), current_(std::move(initial)) {}

  std::uint64_t size() const override { return current_.size(); }
  void read(std::uint64_t offset, MutableByteView out) override {
    check_range(offset, out.size());
    std::copy_n(current_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
  }
  void write(std::uint64_t offset, ByteView data) override {
    check_range(offset, data.size());
    constexpr std::uint64_t kBlock = 4096;
    std::size_t done = 0;
    while (done < data.size()) {
      std::uint64_t at = offset + done;
      std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(data.size() - done, kBlock - at % kBlock));
      fail_if_due();
      std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(done), n,
                  current_.begin() + static_cast<std::ptrdiff_t>(at));
      events_.push_back({false, at, Bytes(data.begin() + static_cast<std::ptrdiff_t>(done),
                                          data.begin() + static_cast<std::ptrdiff_t>(done + n))});
      done += n;
    }
  }
  void flush() override {
    fail_if_due();
    events_.push_back({true, 0, {}});
  }
  using Store::read;

  // From event number `n` on (counting from 0), every write and flush fails.
  void fail_from(std::size_t n) { fail_from_ = n; }

  const std::vector<Event>& events() const { return events_; }
  const Bytes& initial() const { return initial_; }

 private:
  void fail_if_due() {
    if (events_.size() >= fail_from_) throw Error(Errc::io, "simulated disk failure", EIO);
  }

  Bytes initial_;
  Bytes current_;
  std::vector<Event> events_;
  std::size_t fail_from_ = SIZE_MAX;
};

// Walks events in order, keeping the durable image (everything before the
// latest flush) and the writes pending since, so images for successive crash
// points are cheap to build.
class CrashImages {
 public:
  explicit CrashImages(const RecordingStore& store) : store_(store), durable_(store.initial()) {}

  // Advance so that the first `n` events have happened.
  void advance_to(std::size_t n) {
    for (; done_ < n; ++done_) {
      const auto& e = store_.events()[done_];
      if (e.flush) {
        for (std::size_t i : pending_) apply(durable_, store_.events()[i], SIZE_MAX);
        pending_.clear();
      } else {
        pending_.push_back(done_);
      }
    }
  }
  std::size_t pending() const { return pending_.size(); }

  // keep(i) says whether pending write i reached the disk, and how many
  // bytes of it (SIZE_MAX: all of it).
  Bytes image(const std::function<std::size_t(std::size_t)>& keep) const {
    Bytes img = durable_;
    for (std::size_t i = 0; i < pending_.size(); ++i) apply(img, store_.events()[pending_[i]], keep(i));
    return img;
  }

 private:
  static void apply(Bytes& img, const RecordingStore::Event& e, std::size_t bytes) {
    std::size_t n = std::min(bytes, e.data.size());
    std::copy_n(e.data.begin(), n, img.begin() + static_cast<std::ptrdiff_t>(e.offset));
  }

  const RecordingStore& store_;
  Bytes durable_;
  std::vector<std::size_t> pending_;
  std::size_t done_ = 0;
};

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;      // commits only at sync, so states are well defined
  o.count_lookups = false;
  o.threads = 1;
  o.safe_space = 1 << 20;
  o.checkpoint_bytes = 384 << 10;  // checkpoints, and so segment reuse, happen often
  return o;
}

// The complete visible state, as text: equal strings mean equal filesystems.
void dump_dir(Filesystem& fs, InodeId dir, const std::string& path, std::ostringstream& out) {
  for (const DirEntry& e : fs.list(dir)) {
    if (e.name == "." || e.name == "..") continue;
    Attr a = fs.getattr(e.ino);
    std::string p = path + "/" + e.name;
    out << p << " ino=" << a.ino << " mode=" << std::oct << a.mode << std::dec << " nlink=" << a.nlink
        << " uid=" << a.uid << " size=" << a.size << " mtime=" << a.mtime_ns << " ctime=" << a.ctime_ns;
    for (const std::string& x : fs.listxattr(e.ino)) out << " xattr:" << x << "=" << to_hex(fs.getxattr(e.ino, x));
    if (S_ISREG(a.mode)) {
      Bytes data(a.size);
      fs.read(e.ino, 0, data);
      out << " data=" << std::hex << xxh3_64(data) << std::dec;
    } else if (S_ISLNK(a.mode)) {
      out << " target=" << fs.readlink(e.ino);
    }
    out << "\n";
    if (S_ISDIR(a.mode)) dump_dir(fs, e.ino, p, out);
  }
}

std::string dump(Filesystem& fs) {
  std::ostringstream out;
  Attr root = fs.getattr(kRootInode);
  out << "/ nlink=" << root.nlink << " mtime=" << root.mtime_ns << "\n";
  dump_dir(fs, kRootInode, "", out);
  return out.str();
}

// A mix of everything a filesystem does, with a sync after every few steps.
// Large enough to fill segments many times over in a small store, so seals,
// checkpoints and segment reuse all happen. With `gc`, files are also
// copied (so deduplicated), and garbage collection runs after some syncs
// (it changes nothing visible); with `tier`, cold data is recompressed too,
// by itself and by GC (the filesystem must count files as cold at once).
// Throws on the first error.
enum class Extra { none, gc, tier };

void workload(Filesystem& fs, const std::function<void()>& synced, Extra extra = Extra::none) {
  const bool gc = extra != Extra::none;
  std::mt19937_64 rng(42);
  auto pick = [&](std::uint64_t n) { return rng() % n; };
  std::vector<std::string> names = {"a", "b", "c", "d", "e", "f"};
  InodeId dir = fs.mkdir(kRootInode, "dir", 0755, {}).ino;
  for (int step = 0; step < 300; ++step) {
    InodeId parent = pick(3) == 0 ? dir : kRootInode;
    std::string name = names[pick(names.size())];
    int op = static_cast<int>(pick(gc ? 11 : 10));
    try {
      if (op < 4) {
        InodeId f;
        try {
          f = fs.lookup(parent, name).ino;
        } catch (const Error&) {
          f = fs.mknod(parent, name, S_IFREG | 0644, 0, {}).ino;
        }
        if (!S_ISREG(fs.getattr(f).mode)) continue;
        Bytes data(1 + pick(120000));
        bool noisy = pick(4) != 0;  // mostly incompressible, so segments fill up
        for (auto& b : data) b = noisy ? static_cast<std::uint8_t>(rng()) : static_cast<std::uint8_t>('a' + step % 5);
        if (!noisy && extra == Extra::tier) {
          // Text: what a higher level gains from.
          static const char* const words[] = {"error ", "warning ", "request ", "from ", "user ", "id=", "\n", "ok "};
          std::string text;
          while (text.size() < data.size()) text += words[pick(8)] + std::to_string(pick(1000));
          std::copy(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(data.size()), data.begin());
        }
        fs.write(f, pick(80000), data);
      } else if (op == 4) {
        // (One pick per statement: the order arguments are evaluated in is
        // up to the compiler, and the workload must be the same with all.)
        InodeId to = pick(2) ? dir : kRootInode;
        std::string new_name = names[pick(names.size())];
        fs.rename(parent, name, to, new_name, 0);
      } else if (op == 5) {
        fs.unlink(parent, name);
      } else if (op == 6) {
        InodeId f = fs.lookup(parent, name).ino;
        if (S_ISREG(fs.getattr(f).mode)) fs.setattr(f, {.size = pick(70000)});
      } else if (op == 7) {
        fs.link(fs.lookup(parent, name).ino, parent == dir ? kRootInode : dir, name + "2");
      } else if (op == 8) {
        fs.setxattr(fs.lookup(parent, name).ino, "user.k", as_bytes(std::to_string(step)), 0);
      } else if (op == 9) {
        fs.symlink(parent, name + "l", "target" + std::to_string(step), {});
      } else {
        InodeId from = fs.lookup(parent, name).ino;
        if (!S_ISREG(fs.getattr(from).mode)) continue;
        Bytes data(fs.getattr(from).size);
        fs.read(from, 0, data);
        std::string to = names[pick(names.size())] + "c";
        InodeId copy;
        try {
          copy = fs.lookup(parent, to).ino;
        } catch (const Error&) {
          copy = fs.mknod(parent, to, S_IFREG | 0644, 0, {}).ino;
        }
        fs.setattr(copy, {.size = 0});
        fs.write(copy, 0, data);
      }
    } catch (const Error& e) {
      if (e.code() != Errc::posix || e.sys_errno() == EIO) throw;
      // ENOENT, EEXIST, EISDIR, ...: an expected outcome of random operations.
    }
    if (step == 120 || step == 220) {
      // Delete everything: whole segments die, so later checkpoints free
      // them and the log reuses them.
      for (InodeId d : {kRootInode, dir}) {
        for (const DirEntry& e : fs.list(d)) {
          if (e.name != "." && e.name != ".." && !S_ISDIR(e.mode)) fs.unlink(d, e.name);
        }
      }
    }
    if (step % 4 == 3) {
      fs.sync();
      synced();
      if (gc && step % 24 == 23) fs.collect_garbage(0.97);
      if (extra == Extra::tier && step % 24 == 11) fs.recompress_cold(0);
    }
  }
}

struct Recording {
  std::unique_ptr<RecordingStore> store;
  std::vector<std::string> states;        // state after each sync (index 0: at mount)
  std::vector<std::size_t> synced_after;  // events that had happened when each sync returned
};

Bytes fresh_image() {
  MemStore store(12 << 20);  // 11 segments of 1 MiB: reuse is unavoidable
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14});
  return store.image();
}

Recording record_workload(Extra extra = Extra::none) {
  const bool gc = extra != Extra::none;
  Recording r;
  r.store = std::make_unique<RecordingStore>(fresh_image());
  Filesystem::Options o = options();
  if (extra == Extra::tier) o.cold_after_s = 0;
  auto fs = Filesystem::mount(*r.store, o);
  r.states.push_back(dump(*fs));
  r.synced_after.push_back(r.store->events().size());
  workload(*fs, [&] {
    r.states.push_back(dump(*fs));
    r.synced_after.push_back(r.store->events().size());
  }, extra);
  FsInfo info = fs->info();
  if (extra == Extra::tier) {
    std::cout << "[ crash ] recompressed " << info.tier.chunks << " chunks: "
              << info.tier.singles << " records, " << info.tier.packs << " packs, " << info.tier.in_place
              << " in place\n";
    EXPECT_GT(info.tier.singles, 20u) << "the workload must recompress data";
    EXPECT_GT(info.tier.in_place, 10u);
  }
  if (gc) {
    std::cout << "[ crash ] GC copied " << info.gc.records_copied << " records, emptied "
              << info.gc.segments_emptied << " segments; " << info.dedup_hits << " chunks deduplicated\n";
    EXPECT_GT(info.dedup_hits, 20u) << "the workload must make dedup share data";
    EXPECT_GT(info.gc.records_copied, 100u) << "the workload must make GC move data";
    EXPECT_GE(info.gc.segments_emptied, 1u);
  }
  // A segment is (re)started when its header block is written.
  const Superblock& sb = fs->superblock();
  std::size_t starts = 0;
  std::set<std::uint64_t> used;
  for (const auto& e : r.store->events()) {
    if (e.flush || e.offset < sb.data_start || (e.offset - sb.data_start) % sb.segment_size() != 0) continue;
    ++starts;
    used.insert((e.offset - sb.data_start) / sb.segment_size());
  }
  std::cout << "[ crash ] workload: " << r.store->events().size() << " block writes and flushes, "
            << r.states.size() - 1 << " syncs, " << starts << " segment starts in " << used.size()
            << " distinct segments, " << info.files << " files\n";
  EXPECT_GT(starts, used.size()) << "the workload must reuse segments";
  // (GC keeps the data compact: it needs fewer distinct segments.)
  EXPECT_GE(used.size(), gc ? 2u : 3u) << "the workload must cross segments";
  fs->abandon();  // no unmount: the crash points are what matter
  return r;
}

// Mounts an image and checks it holds state k or k + 1, where k syncs had
// completed. Returns the state found, or "" after reporting a failure.
std::string check_image(const Recording& r, Bytes image, std::size_t k, const std::string& what) {
  {
    // fsck, run before mounting as after a real power cut, finds nothing wrong.
    MemStore offline(image);
    FsckReport report = fsck(offline, {});
    if (!report.errors.empty()) {
      ADD_FAILURE() << what << ": fsck found errors in a plain crash image: " << report.errors.front();
      return "";
    }
  }
  MemStore store(std::move(image));
  std::string state;
  try {
    auto fs = Filesystem::mount(store, options());
    state = dump(*fs);
    FsInfo info = fs->info();
    fs->abandon();
    // A crash is not damage: it must never be reported as such.
    if (info.log_damaged || info.checkpoint_fallback || info.read_only) {
      ADD_FAILURE() << what << ": a plain crash was taken for damage (log damaged: "
                    << info.log_damaged << ", checkpoint fallback: " << info.checkpoint_fallback
                    << ", read-only: " << info.read_only << ")";
      return "";
    }
  } catch (const std::exception& e) {
    ADD_FAILURE() << what << ": mount failed: " << e.what();
    return "";
  }
  bool ok = state == r.states[k] || (k + 1 < r.states.size() && state == r.states[k + 1]);
  if (!ok) {
    ADD_FAILURE() << what << ": recovered a state that never existed (after " << k << " syncs)\n"
                  << "--- recovered:\n" << state << "--- expected (sync " << k << "):\n" << r.states[k];
    return "";
  }
  return state;
}

std::size_t syncs_done(const Recording& r, std::size_t events) {
  std::size_t k = 0;
  while (k + 1 < r.synced_after.size() && r.synced_after[k + 1] <= events) ++k;
  return k;
}


// Every crash point (sampled, or all with NANOER_EXHAUSTIVE) of a recording.
void explore(const Recording& r) {
  const auto& events = r.store->events();
  ASSERT_GT(r.states.size(), 20u);
  ASSERT_GT(events.size(), 200u) << "the workload should write a lot";

  std::size_t step = exhaustive() ? 1 : 11;
  std::size_t max_singles = exhaustive() ? SIZE_MAX : 4;
  std::size_t images = 0;
  CrashImages crash(*r.store);
  for (std::size_t n = 0; n <= events.size(); n += step) {
    crash.advance_to(n);
    std::size_t k = syncs_done(r, n);
    std::size_t p = crash.pending();
    std::string where = "crash after event " + std::to_string(n) + " of " + std::to_string(events.size());
    auto check = [&](const std::function<std::size_t(std::size_t)>& keep, const std::string& variant) {
      ++images;
      return !check_image(r, crash.image(keep), k, where + ", " + variant).empty();
    };
    if (!check([](std::size_t) { return 0; }, "unflushed writes lost")) return;
    if (p == 0) continue;
    if (!check([](std::size_t) { return SIZE_MAX; }, "unflushed writes kept")) return;
    for (std::size_t j = 0; j < std::min(p, max_singles); ++j) {
      std::size_t lost = p - 1 - j;  // the most recent first: the likeliest to be lost
      if (!check([&](std::size_t i) { return i == lost ? 0 : SIZE_MAX; },
                 "only write " + std::to_string(lost) + " lost")) {
        return;
      }
    }
    const auto& last = events[n - 1];
    if (!last.flush && last.data.size() > 512) {
      for (std::size_t cut = 512; cut < last.data.size(); cut += exhaustive() ? 512 : 2048) {
        if (!check([&](std::size_t i) { return i == p - 1 ? cut : SIZE_MAX; },
                   "last write torn at byte " + std::to_string(cut))) {
          return;
        }
      }
    }
    std::mt19937_64 rng(n);
    for (int trial = 0; trial < (exhaustive() ? 8 : 2); ++trial) {
      auto keep = [&](std::size_t) -> std::size_t {
        int roll = static_cast<int>(rng() % 3);
        return roll == 0 ? 0 : roll == 1 ? SIZE_MAX : 512 * (1 + rng() % 7);
      };
      if (!check(keep, "random mixture " + std::to_string(trial))) return;
    }
  }
  std::cout << "[ crash ] " << images << " crash images checked over " << events.size()
            << " writes and flushes, " << r.states.size() << " syncs\n";
}

}  // namespace

TEST(Crash, EveryCrashPointRecoversARealState) { explore(record_workload()); }

TEST(Crash, GarbageCollectionIsCrashSafe) { explore(record_workload(Extra::gc)); }

TEST(Crash, RecompressingColdDataIsCrashSafe) { explore(record_workload(Extra::tier)); }

TEST(Crash, CrashesDuringMountAreSafeToo) {
  Recording r = record_workload();
  const auto& events = r.store->events();
  CrashImages crash(*r.store);
  std::size_t checked = 0;
  std::size_t stride = std::max<std::size_t>(1, events.size() / (exhaustive() ? 60 : 8));
  for (std::size_t n = 7; n <= events.size(); n += stride) {
    crash.advance_to(n);
    std::size_t k = syncs_done(r, n);
    // Recover once, recording what the mount itself writes.
    RecordingStore during(crash.image([](std::size_t) { return SIZE_MAX; }));
    std::string first;
    try {
      auto fs = Filesystem::mount(during, options());
      first = dump(*fs);
      fs->abandon();
    } catch (const std::exception& e) {
      FAIL() << "mount after event " << n << " failed: " << e.what();
    }
    // Then crash at every write of that mount: the state must not change.
    CrashImages again(during);
    for (std::size_t m = 0; m <= during.events().size(); ++m) {
      again.advance_to(m);
      for (bool keep : {false, true}) {
        std::string state = check_image(r, again.image([&](std::size_t) { return keep ? SIZE_MAX : 0; }), k,
                                        "crash at event " + std::to_string(m) + " of the mount after event " +
                                            std::to_string(n));
        if (state.empty()) return;
        // Losing the mount's own unflushed writes may only roll back to an
        // earlier real state, never forward past what the first mount saw.
        if (keep) {
          EXPECT_EQ(state, first) << "mount after event " << n << ", crash at " << m;
        }
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 20u);
}

TEST(Crash, DiskErrorsNeverLeaveInconsistentState) {
  Recording full = record_workload();
  std::size_t total = full.store->events().size();
  std::size_t step = exhaustive() ? 1 : std::max<std::size_t>(1, total / 40);
  for (std::size_t fail_at = 0; fail_at < total; fail_at += step) {
    RecordingStore store(full.store->initial());
    store.fail_from(fail_at);
    // The states of this run (timestamps differ from the recorded run).
    Recording r;
    std::size_t syncs = 0;
    try {
      auto fs = Filesystem::mount(store, options());
      r.states.push_back(dump(*fs));
      bool errored = false;
      try {
        workload(*fs, [&] {
          ++syncs;
          r.states.push_back(dump(*fs));
        });
      } catch (const Error& e) {
        errored = true;
        // The commit that failed may still have become durable (say, the
        // error hit the checkpoint after it): what it was writing is the
        // filesystem's state right now, so that is acceptable too.
        r.states.push_back(dump(*fs));
        // From now on, every change is refused.
        int err = 0;
        try {
          fs->mknod(kRootInode, "after-error", S_IFREG | 0644, 0, {});
        } catch (const Error& again) {
          err = again.to_errno();
        }
        EXPECT_TRUE(err == EIO) << "write error at event " << fail_at << ": change accepted after "
                                << e.what();
      }
      EXPECT_TRUE(errored) << "a failing store went unnoticed (fail at " << fail_at << ")";
      fs->abandon();
    } catch (const Error& e) {
      // The failure hit during mount itself: nothing new is durable.
      EXPECT_EQ(e.code(), Errc::io) << e.what();
      MemStore before(full.store->initial());
      auto fs = Filesystem::mount(before, options());
      r.states.push_back(dump(*fs));
      fs->abandon();
    }
    // What the disk durably holds: everything up to the last good flush.
    CrashImages crash(store);
    crash.advance_to(store.events().size());
    std::string state =
        check_image(r, crash.image([](std::size_t) { return 0; }), syncs,
                    "after a disk error at event " + std::to_string(fail_at));
    if (state.empty()) return;
  }
}
