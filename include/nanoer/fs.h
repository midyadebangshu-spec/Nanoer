// The filesystem: POSIX semantics on top of the metadata, the journal and
// the safe-space cache. It has a plain C++ API with no FUSE dependency, so
// it can be tested in-process; src/fuse/ adapts it to the kernel.
//
// Errors are thrown as nanoer::Error; ordinary filesystem errors use
// Errc::posix with the errno (ENOENT, EEXIST, ...). See docs/M2_PLAN.md for
// the behaviour and KNOWLEDGE.md for the reasoning.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "nanoer/gc.h"
#include "nanoer/journal.h"
#include "nanoer/lookup_counts.h"
#include "nanoer/metadata.h"
#include "nanoer/policy.h"
#include "nanoer/stats.h"
#include "nanoer/thread_pool.h"
#include "nanoer/tier.h"

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
  std::uint64_t metadata_bytes = 0;  // the metadata tree's nodes on disk
  std::uint64_t segments_in_use = 0;
  std::uint64_t segments_pending_free = 0;
  std::uint64_t segments_free = 0;
  std::uint64_t segment_bytes = 0;  // data a segment holds
  GcStats gc;                       // since mounting
  TierStats tier;                   // since mounting
  bool cold_data = false;           // cold data is recompressed (the feature, and not turned off)
  bool dedup = false;               // new data is deduplicated
  std::uint64_t dedup_entries = 0;  // contents in the dedup index
  std::uint64_t dedup_index_bytes = 0;
  std::uint64_t dedup_hits = 0;     // chunks stored as a reference, since mounting
  std::uint64_t dedup_bytes = 0;    // their uncompressed size
  std::uint64_t last_txid = 0;
  std::uint64_t replayed_transactions = 0;  // at mount
  std::uint64_t log_bytes_since_checkpoint = 0;
  std::uint64_t checkpoint_bytes = 0;  // size of the checkpoint mounted from
  std::uint64_t cache_resident = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  bool read_only = false;
  bool errors_flag = false;          // the superblock says damage was seen before
  bool errors_seen = false;          // damage seen since mounting
  std::uint64_t read_errors = 0;     // reads that met damaged data
  bool log_damaged = false;          // replay found damage (mounted read-only)
  bool checkpoint_fallback = false;  // the newest checkpoint was damaged; mounted from the previous
  std::uint64_t segments_discarded = 0;  // free segments given back to the layer below, since mounting
  std::uint64_t discard_queued = 0;      // free segments still to give back
};

class Filesystem {
 public:
  struct Options {
    std::uint64_t safe_space = 256 << 20;    // decompressed chunk cache
    std::uint64_t metadata_cache = 64 << 20;  // metadata tree nodes
    unsigned commit_interval_s = 5;          // 0: commit only on sync/fsync/limits
    std::uint64_t dirty_limit = 64 << 20;    // unwritten data before a commit is forced
    std::uint64_t checkpoint_bytes = 256 << 20;  // log written before a checkpoint
    unsigned checkpoint_interval_s = 300;    // ... or this long, if anything was written
    unsigned threads = 0;                    // compression threads; 0: one per CPU
    unsigned gc_idle_s = 10;                 // background GC after this long without changes
    // Data of files unchanged this long (seconds) is recompressed in its
    // cold form, in the background when idle; -1: never. The default
    // policy's setting; `max` folders use at most a day.
    std::int64_t cold_after_s = 7 * 24 * 60 * 60;
    bool dedup = true;  // store identical chunks once (needs shared records, and no no_dedup preference)
    bool discard = true;  // give freed segments back (holes in an image file, TRIM); -o nodiscard
    std::uint64_t dedup_memory = 128 << 20;  // the dedup index's limit (beyond it: missed duplicates)
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
  // Where the space goes (stats.cpp). `by_codec` reads every record header.
  // Also served as the extended attribute user.nanoer.stats of the root.
  SpaceStats space_stats(bool by_codec = false);

  // Garbage collection until no segment at most `max_utilization` full is
  // left, then the two checkpoints that free what it emptied (nanoer gc).
  // Returns how many more segments are free than before. It also runs by
  // itself: in the background, and before any ENOSPC.
  std::uint64_t collect_garbage(double max_utilization = 0.9);
  // Recompresses all cold data now (nanoer tier), as files unchanged for
  // `cold_after_s` (default: the mount's setting) count as cold, then writes
  // the checkpoints that free the old copies. Returns chunks made cold.
  std::uint64_t recompress_cold(std::optional<std::int64_t> cold_after_s = std::nullopt);
  // The dedup index starts empty at mount and is filled from the files'
  // chunks in the background (duplicates written before are missed until
  // then). This fills it now: tests and tools.
  void rebuild_dedup();
  // Commits, then writes a checkpoint now (tools and benchmarks; the
  // filesystem does it by itself when the log has grown or time has passed).
  void checkpoint_now();
  // Gives up to `max` freed segments back to the layer below now (the
  // background does it a few at a time). Returns how many.
  std::size_t discard_freed(std::size_t max = SIZE_MAX);
  bool read_only() const { return read_only_; }
  std::uint32_t chunk_size() const { return sb_.chunk_size(); }
  const Superblock& superblock() const { return sb_; }

 private:
  friend struct FilesystemTestAccess;  // tests: GC steps one phase at a time

  Filesystem(Store& store, const Superblock& sb, ReplayResult replayed, const Options& options,
             bool read_only);

  // Everything below expects mutex_ to be held (unique unless noted).
  // Attributes: as changed since the last commit, else as stored. edit()
  // keeps a copy to be written at the next commit (which admit() may start:
  // call it first).
  std::optional<InodeAttrs> find(InodeId ino) const;  // shared lock is enough
  InodeAttrs get(InodeId ino) const;                  // shared lock is enough; ENOENT
  InodeAttrs get_dir(InodeId ino) const;              // ... ENOTDIR
  InodeAttrs& edit(InodeId ino);
  InodeAttrs& edit_dir(InodeId ino);
  std::vector<InodeId> orphans() const;
  bool has_data(InodeId ino, std::uint64_t index) const;  // stored or not yet
  void check_writable() const;
  void record(Op op);
  void touch(InodeAttrs& inode, bool content);
  void count_lookup(InodeId ino);
  void maybe_free(InodeId ino);
  bool is_ancestor(InodeId ancestor, InodeId dir) const;
  Attr make_attr(const InodeAttrs& inode) const;  // shared lock is enough
  Attr create(InodeId parent, std::string_view name, std::uint32_t mode, std::uint64_t rdev,
              std::string_view target, const Caller& caller);
  Bytes load_chunk(InodeId ino, std::uint64_t index);  // shared lock is enough
  // A chunk's bytes, and what keeps them alive (the cached record: a
  // pack's piece is not copied out of it).
  struct StoredChunk {
    std::shared_ptr<const Bytes> owner;
    ByteView data;
  };
  StoredChunk read_chunk(const ChunkRef& ref);  // shared lock is enough
  Bytes& dirty_chunk(InodeId ino, std::uint64_t index);
  void drop_dirty(InodeId ino, std::uint64_t first_index);
  void truncate(InodeAttrs& file, std::uint64_t size);
  std::uint64_t node_limit() const;        // changed tree nodes before a checkpoint is due
  std::uint64_t reserve_segments() const;  // what normal writes leave free
  void admit(std::uint64_t bytes);
  bool checkpoints_free_space() const;  // two checkpoints would free a segment
  std::uint64_t available_bytes() const;
  void maybe_commit();
  void commit();
  // A live record with exactly this content, confirmed by its full BLAKE3.
  // With its one reference (from its header) if it is not shared yet.
  std::optional<std::pair<ChunkRef, std::optional<ChunkUser>>> find_duplicate(const Blake3Hash& id, ByteView data);
  void maybe_checkpoint();
  void checkpoint();
  SpaceStats space_stats_locked(bool by_codec);  // shared lock is enough
  void stop_background();
  void background();

  // Garbage collection (gc.cpp). A step chooses victims and their live
  // records (shared lock), reads the records (no lock), then copies them
  // and re-points what still points at them (unique lock).
  struct GcItem {
    struct User {
      InodeId ino;
      std::uint64_t index;
      ChunkRef ref;
    };
    std::uint64_t offset;
    std::uint64_t seq;    // of its segment, when chosen
    std::uint64_t limit;  // where its segment ends
    std::uint32_t stored_size;
    std::vector<User> users;
    StorageForm form{};  // packs built again: their users' policy (cold form if cold)
  };
  struct GcPlan {
    std::vector<GcVictim> victims;
    std::vector<GcItem> items;  // in offset order
    // Not copied: cold files whose data in the victims is not cold yet. It
    // is recompressed instead (a tiering step), not copied first.
    std::vector<InodeId> cold_files;
    std::vector<std::pair<NodeRef, NodeInfo>> nodes;  // tree nodes in the victims
    // Victims whose scan stopped before their seal: a damaged record header
    // hides the rest, so they are not collected (records there would be lost).
    std::vector<GcVictim> blind;
    bool empty() const { return items.empty() && nodes.empty() && blind.empty(); }
  };
  struct GcRead {
    std::optional<Record> record;  // the record to copy, intact
    // A mostly dead pack: its live pieces packed again, and where each
    // moved (old piece offset -> new).
    std::optional<EncodedChunk> repacked;
    std::map<std::uint32_t, std::uint32_t> pieces;
  };
  enum class GcNeed { none, idle, low_space };
  GcNeed gc_need() const;  // shared lock is enough
  // `divert`: leave cold files' data that is not cold yet to tiering.
  GcPlan gc_plan(const GcPolicy& policy, bool urgent, bool divert = true) const;  // shared lock is enough
  std::vector<GcRead> gc_read(const GcPlan& plan);  // no lock needed
  bool gc_apply(const GcPlan& plan, std::vector<GcRead>& reads, bool urgent);
  bool gc_step_locked(const GcPolicy& policy, bool urgent);
  void gc_checkpoints(bool low_space);
  void background_gc();

  // Cold data (tier.cpp). A pass first lists the candidate files (and the
  // packs and shared records whose users are all cold); each step then
  // takes some of them (shared lock), reads and recompresses their data (no
  // lock), and re-points the chunks that still refer to what it read
  // (unique lock), like a GC step.
  struct TierMember {
    InodeId ino;
    std::uint64_t index;
    ChunkRef ref;
    std::uint64_t seq;    // of the record's segment, when planned
    std::uint64_t limit;  // where that segment ends
  };
  struct TierItem {
    enum class Kind { single, pack };
    Kind kind{};
    // Every reference to the record (a chunk only one file refers to, a
    // record shared through dedup, or a pack of small files).
    std::vector<TierMember> members;
    StorageForm form{};
  };
  struct TierPlan {
    std::vector<TierItem> items;
    std::uint64_t bytes = 0;  // uncompressed
  };
  struct TierRead {
    bool ok = false;
    bool in_place = false;  // no smaller: mark cold as it is
    EncodedChunk encoded;
    std::map<std::uint32_t, std::uint32_t> moved;  // pack: old piece -> new
    // single: the old record's header fields, kept
    InodeId owner = 0;
    std::uint64_t file_offset = 0;
    std::optional<Blake3Hash> blake3;
    std::uint64_t stored_before = 0;
  };
  bool cold_enabled() const;
  // The file's data is cold now: its form if so.
  std::optional<StorageForm> cold_now(const InodeAttrs& file, std::int64_t now_ns,
                                      std::optional<std::int64_t> age_s) const;
  bool tier_eligible(const TierMember& m) const;  // still as planned
  void tier_begin_pass(std::int64_t now_ns, std::optional<std::int64_t> age_s);  // tier_mutex_, shared lock
  // `only`: plan just these files (GC sends the cold files it meets).
  TierPlan tier_plan(std::uint64_t budget, std::int64_t now_ns, std::optional<std::int64_t> age_s,
                     const std::vector<InodeId>* only = nullptr);  // shared lock
  std::vector<TierRead> tier_read(const TierPlan& plan);  // no lock
  std::uint64_t tier_apply(const TierPlan& plan, std::vector<TierRead>& reads);
  bool tier_room(const TierPlan& plan, bool background) const;
  std::uint64_t tier_files_locked(const std::vector<InodeId>& files);  // GC's cold files
  void background_tier();

  Store& store_;
  Superblock sb_;
  Options options_;
  bool read_only_;
  bool dedup_;
  // One step of rebuilding the dedup index (about `items` tree items),
  // taking the locks itself. False once it is complete.
  bool dedup_step(std::size_t items);
  std::optional<TreeKey> dedup_cursor_;  // where the rebuild goes on; nothing: complete
  std::uint64_t replayed_transactions_;
  std::uint64_t checkpoint_bytes_;

  mutable std::shared_mutex mutex_;  // metadata, write buffer, journal
  Metadata meta_;
  std::unique_ptr<Journal> journal_;
  std::shared_ptr<LogNodeIO> node_io_;  // the tree's nodes in the log (writes go through journal_)
  ChunkCache cache_;
  std::unique_ptr<ThreadPool> pool_;

  // Data written but not yet committed: whole chunks, by file and index.
  std::unordered_map<InodeId, std::map<std::uint64_t, Bytes>> dirty_;
  std::uint64_t dirty_chunks_ = 0;
  std::vector<Op> pending_ops_;             // applied in memory, not yet in the log
  std::uint64_t pending_bytes_ = 0;         // their (approximate) encoded size
  std::unordered_map<InodeId, InodeAttrs> attrs_;  // changed attributes, written at the next commit
  bool failed_ = false;  // a write to the store failed; refuse further changes
  std::atomic<bool> errors_seen_{false};  // damage seen: set the superblock's errors flag
  std::atomic<std::uint64_t> read_errors_{0};
  LogDamage damage_;
  bool checkpoint_fallback_ = false;
  bool in_checkpoint_ = false;
  std::int64_t last_commit_ns_ = 0;
  std::int64_t last_checkpoint_ns_ = 0;
  std::int64_t last_change_ns_ = 0;  // by a user of the filesystem (not GC)
  bool in_gc_ = false;
  bool gc_nodes_moved_ = false;  // tree nodes to be rewritten by the next checkpoint  // GC or tiering is changing the metadata (not a user's change)
  GcStats gc_stats_;
  std::uint64_t gc_emptied_ = 0;  // segments GC emptied since the last checkpoints
  std::uint64_t dedup_hits_ = 0;
  std::uint64_t dedup_bytes_ = 0;
  std::set<std::pair<std::uint64_t, std::uint64_t>> gc_skip_;  // segments with damaged data
  TierStats tier_stats_;
  std::mutex tier_mutex_;  // one pass at a time; guards the pass state below
  std::vector<InodeId> tier_queue_;  // files left in this pass, last first
  std::vector<TierItem> tier_shared_;  // packs and shared records, likewise
  bool tier_pass_open_ = false;
  std::int64_t tier_pass_ns_ = 0;  // when the last pass began
  std::set<std::pair<std::uint64_t, std::uint64_t>> tier_skip_;  // records found damaged

  std::mutex counts_mutex_;  // FUSE lookup and open counts
  LookupCounts lookups_;
  LookupCounts opens_;

  std::mutex background_mutex_;
  std::condition_variable background_wake_;
  bool background_stop_ = false;
  std::thread background_;
  bool unmounted_ = false;
};

}  // namespace nanoer
