// The filesystem: POSIX semantics on top of the metadata, the journal and
// the safe-space cache. It has a plain C++ API with no FUSE dependency, so
// it can be tested in-process; src/fuse/ adapts it to the kernel.
//
// Errors are thrown as nanoer::Error; ordinary filesystem errors use
// Errc::posix with the errno (ENOENT, EEXIST, ...). See docs/M2_PLAN.md for
// the behaviour and KNOWLEDGE.md for the reasoning.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nanoer/bytes.h"
#include "nanoer/cache.h"
#include "nanoer/format.h"
#include "nanoer/journal.h"
#include "nanoer/metadata.h"
#include "nanoer/thread_pool.h"

namespace nanoer {

class Store;

struct Attr {
  InodeId ino = 0;
  std::uint32_t mode = 0;
  std::uint32_t nlink = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint64_t size = 0;
  std::uint64_t blocks = 0;  // 512-byte units of (uncompressed) data, as Btrfs reports
  std::uint64_t rdev = 0;
  std::uint32_t blksize = 0;
  std::int64_t atime_ns = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t ctime_ns = 0;
  std::int64_t btime_ns = 0;
};

// Who is asking: owner of newly created files.
struct Caller {
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
};

struct DirEntry {
  std::string name;
  InodeId ino;
  std::uint32_t mode;  // file type bits
};

struct SetAttrRequest {
  enum class Time { keep, now, set };
  std::optional<std::uint32_t> mode;
  std::optional<std::uint32_t> uid;
  std::optional<std::uint32_t> gid;
  std::optional<std::uint64_t> size;
  Time atime = Time::keep;
  Time mtime = Time::keep;
  std::int64_t atime_ns = 0;
  std::int64_t mtime_ns = 0;
};

struct FsStats {
  std::uint64_t block_size = 0;
  std::uint64_t total_blocks = 0;
  std::uint64_t free_blocks = 0;
  std::uint64_t available_blocks = 0;  // free, minus the space kept for checkpoints
  std::uint64_t files = 0;
  std::uint64_t free_files = 0;
  std::uint32_t name_max = 0;
};

struct FsInfo {
  std::uint64_t files = 0;
  std::uint64_t directories = 0;
  std::uint64_t other_inodes = 0;
  std::uint64_t logical_bytes = 0;  // sum of regular file sizes
  std::uint64_t stored_bytes = 0;   // data chunk records still referenced
  std::uint64_t segments_in_use = 0;
  std::uint64_t segments_pending_free = 0;
  std::uint64_t segments_free = 0;
  std::uint64_t last_txid = 0;
  std::uint64_t replayed_transactions = 0;  // at mount
  std::uint64_t log_bytes_since_checkpoint = 0;
  std::uint64_t checkpoint_bytes = 0;  // size of the checkpoint mounted from
  std::uint64_t cache_resident = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  bool read_only = false;
};

class Filesystem {
 public:
  struct Options {
    std::uint64_t safe_space = 256 << 20;    // decompressed chunk cache
    unsigned commit_interval_s = 5;          // 0: commit only on sync/fsync/limits
    std::uint64_t dirty_limit = 64 << 20;    // unwritten data before a commit is forced
    std::uint64_t checkpoint_bytes = 256 << 20;  // log written before a checkpoint
    unsigned checkpoint_interval_s = 300;    // ... or this long, if anything was written
    unsigned threads = 0;                    // compression threads; 0: one per CPU
    bool read_only = false;
    bool count_lookups = true;  // FUSE lookup counting (in-process users turn it off)
    bool start_threads = true;  // false: call start() later (after daemonizing)
  };

  // Loads the superblock, replays the log and, unless read-only, opens the
  // log for writing. Throws Error on a damaged or unsupported filesystem.
  static std::unique_ptr<Filesystem> mount(Store& store, const Options& options);
  ~Filesystem();  // unmount() unless abandon() was called
  Filesystem(const Filesystem&) = delete;
  Filesystem& operator=(const Filesystem&) = delete;

  void start();    // background commits and compression threads
  void unmount();  // commit everything, write a checkpoint; idempotent
  void abandon();  // drop everything unwritten, as a crash would (tests)

  Attr getattr(InodeId ino);
  Attr lookup(InodeId parent, std::string_view name);  // counts a lookup
  void forget(InodeId ino, std::uint64_t count);
  Attr setattr(InodeId ino, const SetAttrRequest& request);
  std::string readlink(InodeId ino);

  Attr mknod(InodeId parent, std::string_view name, std::uint32_t mode, std::uint64_t rdev,
             const Caller& caller);
  Attr mkdir(InodeId parent, std::string_view name, std::uint32_t mode, const Caller& caller);
  Attr symlink(InodeId parent, std::string_view name, std::string_view target,
               const Caller& caller);
  Attr link(InodeId ino, InodeId new_parent, std::string_view new_name);
  void unlink(InodeId parent, std::string_view name);
  void rmdir(InodeId parent, std::string_view name);
  void rename(InodeId parent, std::string_view name, InodeId new_parent,
              std::string_view new_name, unsigned flags);

  void open(InodeId ino);
  void release(InodeId ino);
  std::size_t read(InodeId ino, std::uint64_t offset, MutableByteView out);
  std::size_t write(InodeId ino, std::uint64_t offset, ByteView data);
  void fallocate(InodeId ino, int mode, std::uint64_t offset, std::uint64_t length);
  std::uint64_t lseek(InodeId ino, std::uint64_t offset, int whence);  // SEEK_DATA / SEEK_HOLE
  void fsync(InodeId ino);
  void sync();

  // A snapshot of a directory, including "." and "..".
  std::vector<DirEntry> list(InodeId dir);

  Bytes getxattr(InodeId ino, std::string_view name);
  std::vector<std::string> listxattr(InodeId ino);
  void setxattr(InodeId ino, std::string_view name, ByteView value, int flags);
  void removexattr(InodeId ino, std::string_view name);

  FsStats statfs();
  FsInfo info();
  bool read_only() const { return read_only_; }
  std::uint32_t chunk_size() const { return sb_.chunk_size(); }
  const Superblock& superblock() const { return sb_; }

 private:
  Filesystem(Store& store, const Superblock& sb, ReplayResult replayed, const Options& options,
             bool read_only);

  // Everything below expects mutex_ to be held (unique unless noted).
  Inode& inode(InodeId ino);
  Inode& directory(InodeId ino);
  void check_writable() const;
  void record(Op op);
  void mark(InodeId ino);
  void touch(Inode& inode, bool content);
  void count_lookup(InodeId ino);
  void maybe_free(InodeId ino);
  bool is_ancestor(InodeId ancestor, InodeId dir) const;
  Attr make_attr(const Inode& inode) const;  // shared lock is enough
  Attr create(InodeId parent, std::string_view name, std::uint32_t mode, std::uint64_t rdev,
              std::string_view target, const Caller& caller);
  Bytes load_chunk(const Inode& file, std::uint64_t index);  // shared lock is enough
  std::shared_ptr<const Bytes> read_chunk(const ChunkRef& ref);  // shared lock is enough
  Bytes& dirty_chunk(Inode& file, std::uint64_t index);
  void drop_dirty(InodeId ino, std::uint64_t first_index);
  void truncate(Inode& file, std::uint64_t size);
  void admit(std::uint64_t bytes);
  std::uint64_t available_bytes() const;
  void maybe_commit();
  void commit();
  void maybe_checkpoint();
  void checkpoint();
  void stop_background();
  void background();

  Store& store_;
  Superblock sb_;
  Options options_;
  bool read_only_;
  std::uint64_t replayed_transactions_;
  std::uint64_t checkpoint_bytes_;

  mutable std::shared_mutex mutex_;  // metadata, write buffer, journal
  Metadata meta_;
  std::unique_ptr<Journal> journal_;
  ChunkCache cache_;
  std::unique_ptr<ThreadPool> pool_;

  // Data written but not yet committed: whole chunks, by file and index.
  std::unordered_map<InodeId, std::map<std::uint64_t, Bytes>> dirty_;
  std::uint64_t dirty_chunks_ = 0;
  std::vector<Op> pending_ops_;             // applied in memory, not yet in the log
  std::uint64_t pending_bytes_ = 0;         // their (approximate) encoded size
  std::unordered_set<InodeId> dirty_inodes_;  // attributes to write at the next commit
  bool failed_ = false;  // a write to the store failed; refuse further changes
  bool in_checkpoint_ = false;
  std::int64_t last_commit_ns_ = 0;
  std::int64_t last_checkpoint_ns_ = 0;

  std::mutex counts_mutex_;  // FUSE lookup and open counts
  std::unordered_map<InodeId, std::uint64_t> lookups_;
  std::unordered_map<InodeId, std::uint64_t> opens_;

  std::mutex background_mutex_;
  std::condition_variable background_wake_;
  bool background_stop_ = false;
  std::thread background_;
  bool unmounted_ = false;
};

}  // namespace nanoer
