#include "nanoer/fs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <tuple>

#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

constexpr std::uint32_t kMaxLinks = 65000;
constexpr std::uint64_t kMaxFileSize = std::uint64_t{1} << 62;
constexpr std::uint64_t kAttrOpBytes = 64;  // rough encoded size of a PutInode
// Segments normal writes never use: room for a checkpoint (which is what
// frees space) and for continuing the log at the next mount.
constexpr std::uint64_t kMinReserveSegments = 3;
// Small files: files up to this size (and one chunk) written in the same
// commit are compressed together, in packs of up to kPackSize bytes
// (docs/M4_PLAN.md §5; measured in KNOWLEDGE.md §14). Pieces start below
// 64 KiB, which the dedup index relies on.
constexpr std::uint64_t kPackFileLimit = 16 << 10;
constexpr std::size_t kPackSize = 64 << 10;
static_assert(kPackSize <= 65536);
// And some that only garbage collection may use, so that it can always copy
// live data out of a victim, and write the checkpoints that free it, even
// when the filesystem is full.
constexpr std::uint64_t kGcHeadroomSegments = 2;
constexpr std::size_t kDiscardBatch = 16;  // segments given back per background tick
constexpr std::uint64_t kMaxDirtyNodeBytes = 32 << 20;

std::int64_t now() { return static_cast<std::int64_t>(now_ns()); }

void check_name(std::string_view name) {
  if (name.size() > kMaxNameLength) throw_posix(ENAMETOOLONG);
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos) {
    throw_posix(EINVAL);
  }
}

bool all_zero(ByteView data) {
  return std::all_of(data.begin(), data.end(), [](std::uint8_t b) { return b == 0; });
}

void log_error(const char* what, const std::exception& e) {
  std::fprintf(stderr, "nanoer: %s: %s\n", what, e.what());
}

}  // namespace

// ---- mount and unmount -----------------------------------------------------

Filesystem::Filesystem(Store& store, const Superblock& sb, ReplayResult replayed,
                       const Options& options, bool read_only)
    : store_(store),
      sb_(sb),
      options_(options),
      read_only_(read_only),
      dedup_(options.dedup && has_shared_records(sb) && !(sb.features_compat & compat_features::no_dedup)),
      replayed_transactions_(replayed.transactions),
      checkpoint_bytes_(replayed.checkpoint_bytes),
      meta_(std::move(replayed.meta)),
      // Shards of at least 4 MiB: each holds many chunks, and its 2Q queue
      // (a quarter) several.
      cache_(options.safe_space,
             static_cast<unsigned>(std::clamp<std::uint64_t>(options.safe_space / (4 << 20), 1, 16))) {
  last_commit_ns_ = last_checkpoint_ns_ = last_change_ns_ = now();
}

std::unique_ptr<Filesystem> Filesystem::mount(Store& store, const Options& options) {
  LoadedSuperblock loaded = load_superblock(store);
  bool read_only = options.read_only || loaded.read_only;
  Superblock sb = loaded.sb;
  bool fell_back = false;
  std::optional<ReplayResult> replayed;
  bool contradiction = false;
  // A transaction that cannot be applied is damage too: use the state
  // before it, read-only.
  const BTree::Options tree{.cache_bytes = options.metadata_cache};
  auto careful = [&](const Superblock& from) {
    try {
      return replay(store, from, UINT64_MAX, tree);
    } catch (const ReplayError& e) {
      std::fprintf(stderr, "nanoer: the log contradicts itself (%s)\n", e.what());
      contradiction = true;
      return replay(store, from, e.txid(), tree);
    }
  };
  try {
    replayed = careful(sb);
  } catch (const CheckpointError& e) {
    // The previous checkpoint and the log after it are still on disk (they
    // are kept until the next checkpoint), so replay from there.
    if (!loaded.older) throw;
    std::fprintf(stderr, "nanoer: the latest checkpoint is damaged (%s); using the previous one\n",
                 e.what());
    sb = *loaded.older;
    replayed = careful(sb);
    fell_back = true;
  }
  if (replayed->legacy && !read_only) {
    std::fprintf(stderr, "nanoer: this filesystem was made before metadata moved to disk (M6); mounting "
                         "read-only. Run nanoer upgrade to write to it.\n");
    read_only = true;
  }
  LogDamage damage = replayed->damage;
  if (contradiction) damage = {true, replayed->end.offset, replayed->meta.last_txid + 1};
  if (sb.state & state_flags::errors) {
    std::fprintf(stderr, "nanoer: damage was found on this filesystem before; run nanoer fsck\n");
  }
  if (damage.found) {
    std::fprintf(stderr,
                 "nanoer: the log is damaged at byte %llu: changes after transaction %llu (up to at "
                 "least %llu) cannot be read. Mounting read-only; run nanoer fsck.\n",
                 static_cast<unsigned long long>(damage.offset),
                 static_cast<unsigned long long>(replayed->meta.last_txid),
                 static_cast<unsigned long long>(damage.durable_claim));
    read_only = true;
  }
  LogPosition end = replayed->end;
  LogPosition checkpoint = replayed->checkpoint;
  std::uint64_t next_seq = std::max(sb.next_segment_seq, replayed->max_seq + 1);
  std::vector<std::uint64_t> written = std::move(replayed->written);
  std::unique_ptr<Filesystem> fs(new Filesystem(store, sb, std::move(*replayed), options, read_only));
  fs->damage_ = damage;
  fs->meta_.dedup.set_limit(options.dedup_memory);
  if (fs->dedup_ && !fs->meta_.dedup_complete) fs->dedup_cursor_ = TreeKey{};
  fs->checkpoint_fallback_ = fell_back;
  if (fell_back || damage.found) fs->errors_seen_ = true;

  if (!fs->read_only_) {
    std::unique_lock lock(fs->mutex_);
    fs->journal_ = std::make_unique<Journal>(store, fs->sb_, fs->meta_.segments, next_seq);
    fs->node_io_ = std::dynamic_pointer_cast<LogNodeIO>(fs->meta_.io());
    if (fs->node_io_) fs->node_io_->set_journal(fs->journal_.get());
    if (options.discard) fs->journal_->enable_discard();
    // Segments the mounted checkpoint no longer needs are kept until the
    // next checkpoint, so the previous checkpoint stays usable if this one
    // is damaged later. Only a filesystem too full to write frees them now.
    if (fs->journal_->free_segments() <= kMinReserveSegments) {
      fs->journal_->release_after_mount(checkpoint, end.index);
    }
    if (fs->journal_->free_segments() == 0) {
      // Continuing the log needs one free segment. Rather than refuse to
      // mount a completely full filesystem, offer its data read-only.
      std::fprintf(stderr, "nanoer: no free segment to continue the log; mounting read-only\n");
      fs->journal_.reset();
      fs->read_only_ = true;
    }
  }
  // A mount that fails part-way must not try to finish an unmount (that
  // would write a checkpoint on top of whatever went wrong).
  struct AbandonOnError {
    Filesystem* fs;
    ~AbandonOnError() {
      if (fs) fs->unmounted_ = true;
    }
  } guard{fs.get()};
  if (!fs->read_only_) {
    std::unique_lock lock(fs->mutex_);
    fs->journal_->resume(end, fs->meta_.last_txid);
    fs->journal_->set_reserve(fs->reserve_segments(), kGcHeadroomSegments);
    // Free segments that may still hold data (freed before a crash, or
    // while discard was off): discarded in the background.
    fs->journal_->queue_discard(written);
    // Files deleted while still open when the system went down.
    for (InodeId id : fs->orphans()) fs->maybe_free(id);
    fs->commit();
  }
  if (options.start_threads) fs->start();
  guard.fs = nullptr;
  return fs;
}

void Filesystem::start() {
  std::unique_lock lock(mutex_);
  if (!pool_) {
    unsigned threads = options_.threads ? options_.threads : std::thread::hardware_concurrency();
    // The calling thread works too, so one fewer pool thread is enough.
    pool_ = std::make_unique<ThreadPool>(threads > 1 ? threads - 1 : 0);
  }
  if (!background_.joinable() && !read_only_ && options_.commit_interval_s > 0) {
    background_ = std::thread([this] { background(); });
  }
}

bool Filesystem::dedup_step(std::size_t items) {
  struct Found {
    InodeId ino;
    std::uint64_t index;
    ChunkRef ref;
  };
  std::vector<Found> found;
  TreeKey from;
  std::optional<TreeKey> next;
  {
    std::shared_lock lock(mutex_);
    if (!dedup_cursor_) return false;
    from = *dedup_cursor_;
    next = meta_.chunks_from(from, items, [&](InodeId ino, std::uint64_t index, const ChunkRef& ref) {
      if (ref.hash) found.push_back({ino, index, ref});
    });
  }
  std::unique_lock lock(mutex_);
  if (dedup_cursor_ != from) return dedup_cursor_.has_value();  // another step went first
  // Only what is still there (the lock was let go in between).
  for (const Found& f : found) {
    if (meta_.chunk(f.ino, f.index) == f.ref) meta_.dedup.put(f.ref.hash, {f.ref.offset, f.ref.packed, f.ref.piece});
  }
  dedup_cursor_ = next;
  if (!next) meta_.dedup_complete = true;
  return next.has_value();
}

std::size_t Filesystem::discard_freed(std::size_t max) {
  std::unique_lock lock(mutex_);
  if (read_only_ || failed_ || !journal_) return 0;
  return journal_->discard_some(max);
}

void Filesystem::rebuild_dedup() {
  while (dedup_step(4096)) {
  }
}

void Filesystem::stop_background() {
  {
    std::lock_guard lock(background_mutex_);
    background_stop_ = true;
  }
  background_wake_.notify_all();
  if (background_.joinable()) background_.join();
}

void Filesystem::background() {
  std::unique_lock wait_lock(background_mutex_);
  while (!background_stop_) {
    background_wake_.wait_for(wait_lock, std::chrono::seconds(1));
    if (background_stop_) break;
    wait_lock.unlock();
    try {
      std::unique_lock lock(mutex_);
      if (now() - last_commit_ns_ >= std::int64_t{options_.commit_interval_s} * 1'000'000'000) {
        commit();
      }
      // Damage was seen: record it in the superblock soon, so the next
      // mount and fsck know.
      if (errors_seen_ && !(sb_.state & state_flags::errors)) checkpoint();
      maybe_checkpoint();
    } catch (const std::exception& e) {
      log_error("background commit failed", e);
    }
    try {
      // Freed segments given back to the layer below, a few at a time.
      discard_freed(kDiscardBatch);
    } catch (const std::exception& e) {
      log_error("discarding free segments failed", e);
    }
    try {
      // A slice of the dedup rebuild at a time, so commits are not held up.
      std::int64_t until = now() + 200'000'000;
      while (dedup_step(4096) && now() < until) {
      }
    } catch (const std::exception& e) {
      log_error("rebuilding the dedup index failed", e);
    }
    try {
      background_tier();
    } catch (const std::exception& e) {
      log_error("recompressing cold data failed", e);
    }
    try {
      background_gc();
    } catch (const std::exception& e) {
      log_error("garbage collection failed", e);
    }
    wait_lock.lock();
  }
}

void Filesystem::unmount() {
  stop_background();
  std::unique_lock lock(mutex_);
  if (unmounted_) return;
  unmounted_ = true;
  if (read_only_ || failed_ || !journal_) return;
  // The kernel has let go of everything: deleted files can go now.
  {
    std::lock_guard counts(counts_mutex_);
    lookups_.clear();
    opens_.clear();
  }
  for (InodeId id : orphans()) maybe_free(id);
  commit();
  checkpoint();
}

void Filesystem::abandon() {
  stop_background();
  std::unique_lock lock(mutex_);
  unmounted_ = true;
}

Filesystem::~Filesystem() {
  try {
    unmount();
  } catch (const std::exception& e) {
    log_error("unmount failed", e);
  }
}

// ---- helpers ---------------------------------------------------------------

std::optional<InodeAttrs> Filesystem::find(InodeId ino) const {
  if (auto it = attrs_.find(ino); it != attrs_.end()) return it->second;
  return meta_.attrs(ino);
}

InodeAttrs Filesystem::get(InodeId ino) const {
  std::optional<InodeAttrs> a = find(ino);
  if (!a) throw_posix(ENOENT);
  return *a;
}

InodeAttrs Filesystem::get_dir(InodeId ino) const {
  InodeAttrs a = get(ino);
  if (!a.is_dir()) throw_posix(ENOTDIR);
  return a;
}

InodeAttrs& Filesystem::edit(InodeId ino) {
  auto it = attrs_.find(ino);
  if (it == attrs_.end()) {
    std::optional<InodeAttrs> a = meta_.attrs(ino);
    if (!a) throw_posix(ENOENT);
    it = attrs_.emplace(ino, *a).first;
    pending_bytes_ += kAttrOpBytes;
  }
  last_change_ns_ = now();
  return it->second;
}

InodeAttrs& Filesystem::edit_dir(InodeId ino) {
  if (!get(ino).is_dir()) throw_posix(ENOTDIR);
  return edit(ino);
}

void Filesystem::check_writable() const {
  if (read_only_) throw_posix(EROFS);
  if (failed_) throw_posix(EIO, "the filesystem stopped accepting changes after a write error");
}

void Filesystem::record(Op op) {
  meta_.apply(op);
  if (!in_gc_) last_change_ns_ = now();
  Encoder size;
  encode_op(size, op);
  pending_bytes_ += size.size();
  pending_ops_.push_back(std::move(op));
}

void Filesystem::touch(InodeAttrs& i, bool content) {
  std::int64_t t = now();
  i.ctime_ns = t;
  if (content) i.mtime_ns = t;
}

void Filesystem::count_lookup(InodeId ino) {
  if (!options_.count_lookups) return;
  std::lock_guard lock(counts_mutex_);
  ++lookups_[ino];
}

void Filesystem::maybe_free(InodeId ino) {
  if (read_only_ || ino == kRootInode) return;
  std::optional<InodeAttrs> i = find(ino);
  if (!i || i->nlink != 0) return;
  {
    std::lock_guard lock(counts_mutex_);
    if (lookups_.contains(ino) || opens_.contains(ino)) return;
  }
  drop_dirty(ino, 0);
  attrs_.erase(ino);
  record(op::DeleteInode{ino});
}

std::vector<InodeId> Filesystem::orphans() const {
  std::vector<InodeId> out = meta_.orphans();
  for (const auto& [id, a] : attrs_) {
    if (a.nlink == 0 && id != kRootInode && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

bool Filesystem::is_ancestor(InodeId ancestor, InodeId dir) const {
  for (InodeId at = dir;;) {
    if (at == ancestor) return true;
    if (at == kRootInode) return false;
    std::optional<InodeAttrs> i = find(at);
    if (!i) return false;
    at = i->parent;
  }
}

Attr Filesystem::make_attr(const InodeAttrs& i) const {
  Attr a;
  a.ino = i.id;
  a.mode = i.mode;
  a.nlink = i.nlink;
  a.uid = i.uid;
  a.gid = i.gid;
  InodeCounts counts = meta_.counts(i.id);
  a.size = i.is_dir() ? counts.entries : i.size;
  a.rdev = i.rdev;
  a.blksize = sb_.chunk_size();
  a.atime_ns = i.atime_ns;
  a.mtime_ns = i.mtime_ns;
  a.ctime_ns = i.ctime_ns;
  a.btime_ns = i.btime_ns;
  if (i.is_regular()) {
    std::uint64_t bytes = counts.data_bytes;
    if (auto it = dirty_.find(i.id); it != dirty_.end()) {
      for (const auto& [index, data] : it->second) {
        if (!meta_.chunk(i.id, index)) bytes += data.size();
      }
    }
    a.blocks = (bytes + 511) / 512;
  }
  return a;
}

std::shared_ptr<const Bytes> Filesystem::read_chunk(const ChunkRef& ref) {
  std::uint64_t segment = meta_.segment_of(ref.offset);
  auto it = meta_.segments.find(segment);
  if (it == meta_.segments.end()) throw Error(Errc::corrupt, "chunk in a free segment");
  ChunkCache::Key key{ref.offset, it->second.seq};
  // A pack is kept whole: its pieces are often read together.
  std::shared_ptr<const Bytes> data = cache_.get_or_load(key, [&] {
    Record record = read_record(store_, ref.offset, sb_.segment_offset(segment) + sb_.segment_size(),
                                it->second.seq);
    return std::make_shared<const Bytes>(decode_data_chunk(record));
  });
  if (!ref.packed && data->size() != ref.size) throw Error(Errc::corrupt, "chunk has the wrong size");
  if (!ref.packed) return data;
  if (std::uint64_t{ref.piece} + ref.size > data->size()) throw Error(Errc::corrupt, "piece outside its pack");
  return std::make_shared<const Bytes>(data->begin() + ref.piece, data->begin() + ref.piece + ref.size);
}

Bytes Filesystem::load_chunk(InodeId ino, std::uint64_t index) {
  std::optional<ChunkRef> ref = meta_.chunk(ino, index);
  if (!ref) return {};
  return *read_chunk(*ref);
}

bool Filesystem::has_data(InodeId ino, std::uint64_t index) const {
  if (auto it = dirty_.find(ino); it != dirty_.end() && it->second.contains(index)) return true;
  return meta_.chunk(ino, index).has_value();
}

Bytes& Filesystem::dirty_chunk(InodeId ino, std::uint64_t index) {
  auto& chunks = dirty_[ino];
  auto it = chunks.find(index);
  if (it == chunks.end()) {
    it = chunks.emplace(index, load_chunk(ino, index)).first;
    ++dirty_chunks_;
  }
  return it->second;
}

void Filesystem::drop_dirty(InodeId ino, std::uint64_t first_index) {
  auto it = dirty_.find(ino);
  if (it == dirty_.end()) return;
  auto& chunks = it->second;
  auto from = chunks.lower_bound(first_index);
  dirty_chunks_ -= static_cast<std::uint64_t>(std::distance(from, chunks.end()));
  chunks.erase(from, chunks.end());
  if (chunks.empty()) dirty_.erase(it);
}

void Filesystem::truncate(InodeAttrs& file, std::uint64_t size) {
  if (size > kMaxFileSize) throw_posix(EFBIG);
  if (size == file.size) return;
  const std::uint64_t cs = sb_.chunk_size();
  if (size < file.size) {
    std::uint64_t first_gone = (size + cs - 1) / cs;
    if (meta_.has_chunk_from(file.id, first_gone)) record(op::TruncateChunks{file.id, first_gone});
    drop_dirty(file.id, first_gone);
    // The chunk the new end falls in keeps bytes past it; rewrite it trimmed,
    // so they cannot reappear if the file grows again.
    if (size % cs != 0) {
      std::uint64_t last = size / cs;
      if (has_data(file.id, last)) {
        Bytes& data = dirty_chunk(file.id, last);
        if (data.size() > size - last * cs) data.resize(size - last * cs);
      }
    }
  }
  file.size = size;
  touch(file, true);
}

std::uint64_t Filesystem::available_bytes() const {
  if (!journal_) return 0;
  std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  std::uint64_t free = journal_->free_segments();
  std::uint64_t reserve = journal_->reserve();  // the journal refuses to dip below this
  return (free > reserve ? (free - reserve) * usable : 0) + journal_->current_remaining();
}

void Filesystem::admit(std::uint64_t bytes) {
  // Conservative: dirty data is counted uncompressed, at a full chunk each.
  const std::uint64_t per_chunk = sb_.chunk_size() + kDataChunkHeaderSize;
  auto needed = [&] { return dirty_chunks_ * per_chunk + pending_bytes_ + bytes + (64 << 10); };
  auto short_by = [&] { return needed() > available_bytes() ? needed() - available_bytes() : 0; };
  if (short_by() == 0) return;
  // Write out what is waiting, then free dead segments: they are released by
  // one checkpoint and become free at the next. Checkpoints cost space too
  // (they may leave a nearly full segment for a new one), so only when they
  // free something.
  commit();
  auto free_dead = [&] {
    if (!checkpoints_free_space()) return;
    checkpoint();
    checkpoint();
    gc_emptied_ = 0;
  };
  free_dead();
  // Then collect garbage: copy the live data out of the emptiest segments
  // until what they hold covers the need, and free them the same way.
  const std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  const GcPolicy urgent{.greedy = true, .max_utilization = 0.97};
  for (int round = 0; round < 8 && short_by() > 0; ++round) {
    std::uint64_t emptied = gc_emptied_;
    bool moved = false;
    for (int step = 0; step < 64 && (gc_emptied_ - emptied) * usable < short_by(); ++step) {
      if (!gc_step_locked(urgent, true)) break;
      moved = true;
    }
    if (!moved && !checkpoints_free_space()) break;
    free_dead();
  }
  if (short_by() > 0) throw_posix(ENOSPC);
}

bool Filesystem::checkpoints_free_space() const {
  if (journal_->pending_free_segments() > 0 || gc_nodes_moved_) return true;
  for (const auto& [index, usage] : meta_.segments) {
    if (usage.live_bytes == 0 && index != journal_->current_index()) return true;
  }
  return false;
}

void Filesystem::maybe_commit() {
  if (dirty_chunks_ * sb_.chunk_size() >= options_.dirty_limit ||
      pending_bytes_ >= journal_->max_transaction_payload() / 2) {
    commit();
  }
}

// ---- commits and checkpoints -----------------------------------------------

void Filesystem::commit() {
  if (read_only_ || failed_ || !journal_) return;
  if (dirty_.empty() && pending_ops_.empty() && attrs_.empty()) return;
  try {
    struct Job {
      InodeId ino;
      std::uint64_t index;
      Bytes* data;
      bool hole;
      EncodedChunk encoded;
      Blake3Hash id{};
      std::optional<ChunkRef> duplicate;  // of live data
      std::optional<ChunkUser> owner;     // its one reference, if it has one (it becomes shared)
      std::optional<std::size_t> same_as;  // an earlier job in this commit
      bool packed = false;                 // stored as a piece of a pack
      ChunkRef ref;
      StorageForm form{};                  // the file's policy
    };
    const std::uint64_t cs = sb_.chunk_size();
    std::vector<Job> jobs;
    for (auto& [ino, chunks] : dirty_) {
      std::optional<InodeAttrs> file = find(ino);
      if (!file) continue;
      for (auto& [index, data] : chunks) {
        std::uint64_t start = index * cs;
        if (start >= file->size) continue;
        // The file may have grown past this chunk since it was written:
        // the bytes in between are zeros.
        data.resize(static_cast<std::size_t>(std::min(cs, file->size - start)), 0);
        jobs.push_back({ino, index, &data, all_zero(data), {}});
        jobs.back().form = hot_form(policy_of(meta_, ino), sb_);
      }
    }
    auto parallel = [&](std::size_t n, const std::function<void(std::size_t)>& f) {
      if (pool_) {
        pool_->parallel_for(n, f);
      } else {
        for (std::size_t i = 0; i < n; ++i) f(i);
      }
    };
    auto in_parallel = [&](const std::function<void(std::size_t)>& f) { parallel(jobs.size(), f); };
    // Dedup: identify every chunk first (BLAKE3 is faster than compressing);
    // a chunk whose content is already stored, or stored earlier in this
    // commit, is only referenced.
    if (dedup_) {
      in_parallel([&](std::size_t i) {
        if (!jobs[i].hole) jobs[i].id = blake3(*jobs[i].data);
      });
      std::unordered_map<std::uint64_t, std::size_t> first;
      for (std::size_t i = 0; i < jobs.size(); ++i) {
        Job& job = jobs[i];
        if (job.hole) continue;
        std::uint64_t key = hash_key(job.id.data());
        if (auto it = first.find(key); it != first.end() && jobs[it->second].id == job.id) {
          job.same_as = it->second;
          continue;
        }
        if (auto d = find_duplicate(job.id, *job.data)) {
          job.duplicate = d->first;
          job.owner = d->second;
        }
        if (!job.duplicate) first.emplace(key, i);
      }
    }
    // Small files left to store go into packs: together they compress far
    // better than one by one. In directory and name order where the
    // directory changed in this commit (new files), else by inode number.
    struct Pack {
      std::vector<std::size_t> members;
      Bytes data;
      EncodedChunk encoded;
      StorageForm form{};
    };
    std::vector<Pack> packs;
    if (has_shared_records(sb_)) {
      const std::uint64_t limit = std::min(kPackFileLimit, cs);
      std::vector<std::size_t> small;
      std::unordered_set<InodeId> small_inodes;
      for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Job& job = jobs[i];
        if (job.hole || job.duplicate || job.same_as || job.index != 0) continue;
        if (job.form.codec == CodecId::none) continue;  // stored raw: nothing to gain
        std::optional<InodeAttrs> file = find(job.ino);
        if (file->size > limit || file->size != job.data->size()) continue;
        small.push_back(i);
        small_inodes.insert(job.ino);
      }
      if (small.size() >= 2) {
        std::unordered_map<InodeId, std::pair<InodeId, std::string_view>> where;
        std::vector<std::string> names;  // (kept alive for the string_views below)
        names.reserve(small.size());
        for (const auto& [d, dir] : attrs_) {
          if (!dir.is_dir()) continue;
          meta_.entries(d, [&](std::string_view name, InodeId child) {
            if (small_inodes.contains(child) && !where.contains(child)) {
              names.emplace_back(name);
              where.emplace(child, std::make_pair(d, std::string_view(names.back())));
            }
            return true;
          });
        }
        auto order = [&](std::size_t i) {
          auto w = where.find(jobs[i].ino);
          return w == where.end() ? std::make_tuple(1, jobs[i].ino, std::string_view(), jobs[i].ino)
                                  : std::make_tuple(0, w->second.first, w->second.second, jobs[i].ino);
        };
        std::sort(small.begin(), small.end(), [&](std::size_t a, std::size_t b) { return order(a) < order(b); });
        // Files of one policy (one form) in a pack.
        std::map<std::pair<CodecId, int>, std::size_t> open;
        for (std::size_t i : small) {
          std::pair<CodecId, int> form{jobs[i].form.codec, jobs[i].form.level};
          auto o = open.find(form);
          if (o == open.end() || packs[o->second].data.size() + jobs[i].data->size() > kPackSize) {
            packs.emplace_back();
            packs.back().form = jobs[i].form;
            o = open.insert_or_assign(form, packs.size() - 1).first;
          }
          Pack& p = packs[o->second];
          jobs[i].ref.piece = static_cast<std::uint32_t>(p.data.size());
          p.data.insert(p.data.end(), jobs[i].data->begin(), jobs[i].data->end());
          p.members.push_back(i);
        }
        // A file alone in its pack gains nothing from it.
        std::erase_if(packs, [](const Pack& p) { return p.members.size() < 2; });
        for (const Pack& p : packs) {
          for (std::size_t i : p.members) jobs[i].packed = true;
        }
      }
    }
    in_parallel([&](std::size_t i) {
      Job& job = jobs[i];
      if (!job.hole && !job.duplicate && !job.same_as && !job.packed) {
        job.encoded = encode_chunk(*job.data, job.form.codec, job.form.level);
      }
    });
    parallel(packs.size(), [&](std::size_t i) {
      packs[i].encoded = encode_chunk(packs[i].data, packs[i].form.codec, packs[i].form.level);
    });
    for (Pack& p : packs) {
      ChunkRef pack = journal_->append_chunk(p.encoded, 0, 0, std::nullopt, true);
      for (std::size_t i : p.members) {
        Job& job = jobs[i];
        job.ref = {pack.offset, pack.stored_size, static_cast<std::uint32_t>(job.data->size()), job.ref.piece,
                   true, dedup_ ? hash_key(job.id.data()) : 0};
      }
    }

    std::vector<Op> ops = std::move(pending_ops_);
    pending_ops_.clear();
    for (Job& job : jobs) {
      Op op;
      // An earlier chunk of this commit may have dropped the last reference
      // to the record a duplicate was found in (it is no longer indexed):
      // then this chunk is stored after all.
      if (job.duplicate) {
        std::optional<DedupTarget> t = meta_.dedup.find(job.duplicate->hash);
        if (t != DedupTarget{job.duplicate->offset, job.duplicate->packed, job.duplicate->piece}) {
          job.duplicate.reset();
          job.encoded = encode_chunk(*job.data, job.form.codec, job.form.level);
        }
      }
      if (job.hole) {
        if (!meta_.chunk(job.ino, job.index)) continue;
        op = op::RemoveChunk{job.ino, job.index};  // all zeros: store nothing
      } else if (job.duplicate || job.same_as) {
        job.ref = job.duplicate ? *job.duplicate : jobs[*job.same_as].ref;
        std::optional<ChunkUser> owner;
        if (!job.ref.packed && !meta_.users(job.ref.offset)) {
          // Shared for the first time: name its one reference.
          owner = job.duplicate ? job.owner : ChunkUser{jobs[*job.same_as].ino, jobs[*job.same_as].index};
        }
        op = op::SetChunkRef{job.ino, job.index, job.ref, !job.ref.packed, owner};
        ++dedup_hits_;
        dedup_bytes_ += job.ref.size;
      } else if (job.packed) {
        op = op::SetChunkRef{job.ino, job.index, job.ref, false};  // the first piece claims the pack
      } else if (dedup_) {
        job.ref = journal_->append_chunk(job.encoded, job.ino, job.index * cs, job.id);
        op = op::SetChunkRef{job.ino, job.index, job.ref, false};
      } else {
        job.ref = journal_->append_chunk(job.encoded, job.ino, job.index * cs);
        op = op::SetChunk{job.ino, job.index, job.ref};
      }
      meta_.apply(op);
      ops.push_back(std::move(op));
    }
    for (const auto& [ino, a] : attrs_) {
      if (!meta_.attrs(ino)) continue;  // deleted since
      Op op = op::PutInode{a};
      meta_.apply(op);
      ops.push_back(std::move(op));
    }
    std::uint64_t txid = meta_.last_txid + 1;
    journal_->append_transaction(txid, ops);
    meta_.last_txid = txid;
    journal_->flush();
  } catch (const std::exception& e) {
    // Memory now holds changes the log does not: refuse further changes
    // rather than let the two drift apart.
    failed_ = true;
    log_error("commit failed; the filesystem no longer accepts changes", e);
    throw;
  }
  dirty_.clear();
  dirty_chunks_ = 0;
  attrs_.clear();
  pending_bytes_ = 0;
  last_commit_ns_ = now();
  maybe_checkpoint();
}

std::optional<std::pair<ChunkRef, std::optional<ChunkUser>>> Filesystem::find_duplicate(const Blake3Hash& id,
                                                                                         ByteView data) {
  const auto size = static_cast<std::uint32_t>(data.size());
  std::uint64_t key = hash_key(id.data());
  std::optional<DedupTarget> t = meta_.dedup.find(key);
  if (!t) return std::nullopt;
  std::uint64_t segment = meta_.segment_of(t->offset);
  auto usage = meta_.segments.find(segment);
  if (usage == meta_.segments.end()) return std::nullopt;
  // The key is only a hint: the record's full BLAKE3 decides.
  std::optional<Record> r = try_read_record_header(
      store_, t->offset, sb_.segment_offset(segment) + sb_.segment_size(), usage->second.seq);
  if (!r || r->header.type != RecordType::data_chunk) return std::nullopt;
  ChunkRef ref{t->offset, static_cast<std::uint32_t>(r->stored_size()), size, t->piece, t->packed, key};
  std::optional<ChunkUser> owner;
  try {
    DataChunkInfo info = parse_data_chunk(*r);
    // A record not shared yet has one reference: its header's owner.
    if (!info.pack) owner = ChunkUser{info.inode, info.file_offset / sb_.chunk_size()};
    if (info.pack != t->packed) return std::nullopt;
    if (!t->packed) {
      if (!info.blake3 || *info.blake3 != id || info.uncompressed_size != size) return std::nullopt;
    } else {
      // A pack records no identity for its pieces: compare the bytes (a
      // small file's, from a pack the cache usually still holds).
      if (std::uint64_t{t->piece} + size > info.uncompressed_size) return std::nullopt;
      std::shared_ptr<const Bytes> piece = read_chunk(ref);
      if (!std::equal(piece->begin(), piece->end(), data.begin(), data.end())) return std::nullopt;
    }
  } catch (const Error&) {
    return std::nullopt;
  }
  return std::make_pair(ref, owner);
}

void Filesystem::maybe_checkpoint() {
  if (!journal_ || in_checkpoint_) return;
  std::uint64_t written = journal_->bytes_since_checkpoint();
  bool overdue = written > 0 && now() - last_checkpoint_ns_ >=
                                    std::int64_t{options_.checkpoint_interval_s} * 1'000'000'000;
  if (written >= options_.checkpoint_bytes || overdue || meta_.dirty_bytes() >= node_limit()) checkpoint();
}

std::uint64_t Filesystem::node_limit() const {
  // Changed tree nodes are written by the next checkpoint: bounded, so that
  // memory is, and so is the room a checkpoint needs.
  std::uint64_t store = sb_.segment_count * sb_.segment_size();
  return std::clamp<std::uint64_t>(store / 32, 256 << 10, kMaxDirtyNodeBytes);
}

std::uint64_t Filesystem::reserve_segments() const {
  std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  return kMinReserveSegments + kGcHeadroomSegments + (checkpoint_bytes_ * 3 / 2 + node_limit() + usable - 1) / usable;
}

void Filesystem::checkpoint() {
  if (read_only_ || failed_ || !journal_ || in_checkpoint_) return;
  in_checkpoint_ = true;
  try {
    commit();
    LogPosition at = journal_->append_checkpoint(meta_);
    gc_nodes_moved_ = false;
    journal_->flush();
    sb_.checkpoint_offset = at.offset;
    sb_.checkpoint_seq = at.seq;
    sb_.next_segment_seq = journal_->next_seq();
    if (errors_seen_) sb_.state |= state_flags::errors;
    write_superblock(store_, sb_);
    journal_->checkpoint_done(at);
    // Keep room for the next checkpoint.
    checkpoint_bytes_ = meta_.encode_checkpoint().size();
    journal_->set_reserve(reserve_segments(), kGcHeadroomSegments);
  } catch (const std::exception& e) {
    in_checkpoint_ = false;
    failed_ = true;
    log_error("checkpoint failed; the filesystem no longer accepts changes", e);
    throw;
  }
  in_checkpoint_ = false;
  last_checkpoint_ns_ = now();
}

void Filesystem::sync() {
  std::unique_lock lock(mutex_);
  if (read_only_) return;
  if (failed_) throw_posix(EIO);
  commit();
}

void Filesystem::fsync(InodeId ino) {
  std::unique_lock lock(mutex_);
  get(ino);
  if (read_only_) return;
  if (failed_) throw_posix(EIO);
  commit();  // commits everything before it, not just this file: simple and stronger
}

// ---- attributes and lookups ------------------------------------------------

Attr Filesystem::getattr(InodeId ino) {
  std::shared_lock lock(mutex_);
  return make_attr(get(ino));
}

Attr Filesystem::lookup(InodeId parent, std::string_view name) {
  std::shared_lock lock(mutex_);
  InodeAttrs dir = get_dir(parent);
  if (name.size() > kMaxNameLength) throw_posix(ENAMETOOLONG);
  InodeId ino;
  if (name == ".") {
    ino = parent;
  } else if (name == "..") {
    ino = dir.parent;
  } else {
    std::optional<InodeId> e = meta_.entry(parent, name);
    if (!e) throw_posix(ENOENT);
    ino = *e;
  }
  Attr a = make_attr(get(ino));
  count_lookup(ino);
  return a;
}

void Filesystem::forget(InodeId ino, std::uint64_t count) {
  std::unique_lock lock(mutex_);
  {
    std::lock_guard counts(counts_mutex_);
    auto it = lookups_.find(ino);
    if (it == lookups_.end()) return;
    it->second = it->second > count ? it->second - count : 0;
    if (it->second != 0) return;
    lookups_.erase(it);
  }
  maybe_free(ino);
}

Attr Filesystem::setattr(InodeId ino, const SetAttrRequest& r) {
  std::unique_lock lock(mutex_);
  check_writable();
  {
    InodeAttrs i = get(ino);
    // Shrinking a file frees space, so it must work on a full filesystem too.
    if (!r.size || *r.size >= i.size) admit(kAttrOpBytes);
    if (r.size && i.is_dir()) throw_posix(EISDIR);
    if (r.size && !i.is_regular()) throw_posix(EINVAL);
  }
  InodeAttrs& i = edit(ino);  // (after admit(), which may commit)
  if (r.size) truncate(i, *r.size);
  if (r.mode) i.mode = (i.mode & S_IFMT) | (*r.mode & 07777);
  if (r.uid) i.uid = *r.uid;
  if (r.gid) i.gid = *r.gid;
  std::int64_t t = now();
  if (r.atime == SetAttrRequest::Time::now) i.atime_ns = t;
  if (r.atime == SetAttrRequest::Time::set) i.atime_ns = r.atime_ns;
  if (r.mtime == SetAttrRequest::Time::now) i.mtime_ns = t;
  if (r.mtime == SetAttrRequest::Time::set) i.mtime_ns = r.mtime_ns;
  i.ctime_ns = t;
  Attr a = make_attr(i);
  maybe_commit();
  return a;
}

std::string Filesystem::readlink(InodeId ino) {
  std::shared_lock lock(mutex_);
  InodeAttrs i = get(ino);
  if (!i.is_symlink()) throw_posix(EINVAL);
  return i.symlink;
}

// ---- creating and removing -------------------------------------------------

Attr Filesystem::create(InodeId parent, std::string_view name, std::uint32_t mode,
                        std::uint64_t rdev, std::string_view target, const Caller& caller) {
  check_writable();
  InodeAttrs before = get_dir(parent);
  check_name(name);
  if (meta_.entry(parent, name)) throw_posix(EEXIST);
  bool is_dir = S_ISDIR(mode);
  if (is_dir && before.nlink >= kMaxLinks) throw_posix(EMLINK);
  admit(kAttrOpBytes * 2 + name.size() + target.size());
  InodeAttrs& dir = edit(parent);

  std::int64_t t = now();
  InodeAttrs a;
  a.id = meta_.next_inode;
  a.mode = mode;
  a.uid = caller.uid;
  a.gid = caller.gid;
  // BSD semantics, as Linux uses on a set-group-ID directory: new entries
  // take the directory's group, and new subdirectories inherit the bit.
  if (dir.mode & S_ISGID) {
    a.gid = dir.gid;
    if (is_dir) a.mode |= S_ISGID;
  }
  a.nlink = is_dir ? 2 : 1;
  a.size = target.size();
  a.atime_ns = a.mtime_ns = a.ctime_ns = a.btime_ns = t;
  a.rdev = rdev;
  a.parent = is_dir ? parent : 0;
  a.symlink = std::string(target);
  record(op::PutInode{a});
  // A folder's policy is passed on to what is created in it.
  if (std::optional<Bytes> policy = meta_.xattr(parent, kPolicyXattr); policy && !S_ISLNK(mode)) {
    record(op::SetXattr{a.id, kPolicyXattr, *policy});
  }
  record(op::SetEntry{parent, std::string(name), a.id});
  if (is_dir) ++dir.nlink;
  touch(dir, true);
  count_lookup(a.id);
  Attr result = make_attr(get(a.id));
  maybe_commit();
  return result;
}

Attr Filesystem::mknod(InodeId parent, std::string_view name, std::uint32_t mode,
                       std::uint64_t rdev, const Caller& caller) {
  std::unique_lock lock(mutex_);
  if (S_ISDIR(mode) || S_ISLNK(mode)) throw_posix(EINVAL);
  if ((mode & S_IFMT) == 0) mode |= S_IFREG;
  bool device = S_ISCHR(mode) || S_ISBLK(mode);
  return create(parent, name, mode, device ? rdev : 0, {}, caller);
}

Attr Filesystem::mkdir(InodeId parent, std::string_view name, std::uint32_t mode,
                       const Caller& caller) {
  std::unique_lock lock(mutex_);
  return create(parent, name, S_IFDIR | (mode & 07777), 0, {}, caller);
}

Attr Filesystem::symlink(InodeId parent, std::string_view name, std::string_view target,
                         const Caller& caller) {
  std::unique_lock lock(mutex_);
  if (target.empty()) throw_posix(ENOENT);
  if (target.size() > kMaxSymlinkLength) throw_posix(ENAMETOOLONG);
  return create(parent, name, S_IFLNK | 0777, 0, target, caller);
}

Attr Filesystem::link(InodeId ino, InodeId new_parent, std::string_view new_name) {
  std::unique_lock lock(mutex_);
  check_writable();
  {
    InodeAttrs i = get(ino);
    if (i.is_dir()) throw_posix(EPERM);
    get_dir(new_parent);
    check_name(new_name);
    if (meta_.entry(new_parent, new_name)) throw_posix(EEXIST);
    if (i.nlink >= kMaxLinks) throw_posix(EMLINK);
  }
  admit(kAttrOpBytes * 2 + new_name.size());
  record(op::SetEntry{new_parent, std::string(new_name), ino});
  InodeAttrs& i = edit(ino);
  ++i.nlink;
  touch(i, false);
  touch(edit(new_parent), true);
  count_lookup(ino);
  Attr a = make_attr(i);
  maybe_commit();
  return a;
}

void Filesystem::unlink(InodeId parent, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  get_dir(parent);
  std::optional<InodeId> e = meta_.entry(parent, name);
  if (!e) throw_posix(ENOENT);
  if (get(*e).is_dir()) throw_posix(EISDIR);
  // No space check: deleting is how a full filesystem gets space back. The
  // transaction is tiny and may use the reserve.
  record(op::RemoveEntry{parent, std::string(name)});
  InodeAttrs& target = edit(*e);
  --target.nlink;
  touch(target, false);
  touch(edit(parent), true);
  maybe_free(*e);
  maybe_commit();
}

void Filesystem::rmdir(InodeId parent, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  get_dir(parent);
  if (name == ".") throw_posix(EINVAL);
  if (name == "..") throw_posix(ENOTEMPTY);
  std::optional<InodeId> e = meta_.entry(parent, name);
  if (!e) throw_posix(ENOENT);
  if (!get(*e).is_dir()) throw_posix(ENOTDIR);
  if (meta_.counts(*e).entries != 0) throw_posix(ENOTEMPTY);
  record(op::RemoveEntry{parent, std::string(name)});  // no space check, as for unlink
  InodeAttrs& target = edit(*e);
  InodeAttrs& dir = edit(parent);
  target.nlink = 0;
  --dir.nlink;
  touch(target, false);
  touch(dir, true);
  maybe_free(*e);
  maybe_commit();
}

void Filesystem::rename(InodeId parent, std::string_view name, InodeId new_parent,
                        std::string_view new_name, unsigned flags) {
  std::unique_lock lock(mutex_);
  check_writable();
  if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) throw_posix(EINVAL);
  if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) throw_posix(EINVAL);
  get_dir(parent);
  get_dir(new_parent);
  check_name(new_name);
  std::optional<InodeId> src_entry = meta_.entry(parent, name);
  if (!src_entry) throw_posix(ENOENT);
  InodeId src = *src_entry;
  InodeId dst = meta_.entry(new_parent, new_name).value_or(0);
  if (parent == new_parent && name == new_name) return;
  get(src);
  admit(kAttrOpBytes * 4 + name.size() + new_name.size());
  // (After admit(), which may commit.)
  InodeAttrs& src_dir = edit(parent);
  InodeAttrs& dst_dir = edit(new_parent);
  InodeAttrs& s = edit(src);

  if (flags & RENAME_EXCHANGE) {
    if (!dst) throw_posix(ENOENT);
    InodeAttrs& d = edit(dst);
    if (s.is_dir() && is_ancestor(src, new_parent)) throw_posix(EINVAL);
    if (d.is_dir() && is_ancestor(dst, parent)) throw_posix(EINVAL);
    record(op::SetEntry{parent, std::string(name), dst});
    record(op::SetEntry{new_parent, std::string(new_name), src});
    if (parent != new_parent) {
      if (s.is_dir()) {
        s.parent = new_parent;
        --src_dir.nlink;
        ++dst_dir.nlink;
      }
      if (d.is_dir()) {
        d.parent = parent;
        --dst_dir.nlink;
        ++src_dir.nlink;
      }
    }
    touch(s, false);
    touch(d, false);
    touch(src_dir, true);
    touch(dst_dir, true);
    maybe_commit();
    return;
  }

  if (dst && (flags & RENAME_NOREPLACE)) throw_posix(EEXIST);
  if (s.is_dir() && is_ancestor(src, new_parent)) throw_posix(EINVAL);
  if (dst == src) return;  // two links to the same file: POSIX says do nothing
  if (dst) {
    InodeAttrs d = get(dst);
    if (s.is_dir() && !d.is_dir()) throw_posix(ENOTDIR);
    if (!s.is_dir() && d.is_dir()) throw_posix(EISDIR);
    if (d.is_dir() && meta_.counts(dst).entries != 0) throw_posix(ENOTEMPTY);
  }

  record(op::SetEntry{new_parent, std::string(new_name), src});
  record(op::RemoveEntry{parent, std::string(name)});
  if (dst) {
    InodeAttrs& d = edit(dst);
    if (d.is_dir()) {
      d.nlink = 0;
      --dst_dir.nlink;
    } else {
      --d.nlink;
    }
    touch(d, false);
  }
  if (s.is_dir() && parent != new_parent) {
    s.parent = new_parent;
    --src_dir.nlink;
    ++dst_dir.nlink;
  }
  touch(s, false);
  touch(src_dir, true);
  touch(dst_dir, true);
  if (dst) maybe_free(dst);
  maybe_commit();
}

// ---- file data -------------------------------------------------------------

void Filesystem::open(InodeId ino) {
  std::shared_lock lock(mutex_);
  get(ino);
  std::lock_guard counts(counts_mutex_);
  ++opens_[ino];
}

void Filesystem::release(InodeId ino) {
  std::unique_lock lock(mutex_);
  {
    std::lock_guard counts(counts_mutex_);
    auto it = opens_.find(ino);
    if (it == opens_.end()) return;
    if (--it->second != 0) return;
    opens_.erase(it);
  }
  maybe_free(ino);
}

std::size_t Filesystem::read(InodeId ino, std::uint64_t offset, MutableByteView out) {
  std::shared_lock lock(mutex_);
  InodeAttrs file = get(ino);
  if (file.is_dir()) throw_posix(EISDIR);
  if (!file.is_regular()) throw_posix(EINVAL);
  if (offset >= file.size) return 0;
  std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), file.size - offset));
  const std::uint64_t cs = sb_.chunk_size();
  auto dirty = dirty_.find(ino);
  std::size_t done = 0;
  while (done < n) {
    std::uint64_t pos = offset + done;
    std::uint64_t index = pos / cs;
    std::size_t within = static_cast<std::size_t>(pos - index * cs);
    std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(n - done, cs - within));
    std::uint8_t* dst = out.data() + done;
    // Newest data first: uncommitted writes, then stored chunks, then holes.
    const Bytes* source = nullptr;
    std::shared_ptr<const Bytes> stored;
    if (dirty != dirty_.end()) {
      if (auto c = dirty->second.find(index); c != dirty->second.end()) source = &c->second;
    }
    if (!source) {
      if (std::optional<ChunkRef> c = meta_.chunk(ino, index)) {
        try {
          stored = read_chunk(*c);
        } catch (const Error& e) {
          if (e.code() != Errc::corrupt) throw;
          // Report exactly what is lost; never return damaged data.
          ++read_errors_;
          errors_seen_ = true;
          std::fprintf(stderr, "nanoer: damaged data in inode %llu, bytes %llu-%llu: %s\n",
                       static_cast<unsigned long long>(ino),
                       static_cast<unsigned long long>(index * cs),
                       static_cast<unsigned long long>(index * cs + c->size - 1), e.what());
          throw;
        }
        source = stored.get();
      }
    }
    std::size_t have = source && source->size() > within ? std::min(len, source->size() - within) : 0;
    if (have) std::memcpy(dst, source->data() + within, have);
    std::memset(dst + have, 0, len - have);  // beyond the stored bytes: zeros
    done += len;
  }
  return n;
}

std::size_t Filesystem::write(InodeId ino, std::uint64_t offset, ByteView data) {
  std::unique_lock lock(mutex_);
  check_writable();
  {
    InodeAttrs file = get(ino);
    if (file.is_dir()) throw_posix(EISDIR);
    if (!file.is_regular()) throw_posix(EINVAL);
  }
  if (data.empty()) return 0;
  if (offset > kMaxFileSize || data.size() > kMaxFileSize - offset) throw_posix(EFBIG);
  const std::uint64_t cs = sb_.chunk_size();
  std::uint64_t first = offset / cs;
  std::uint64_t last = (offset + data.size() - 1) / cs;
  admit((last - first + 1) * (cs + kDataChunkHeaderSize));

  std::size_t done = 0;
  for (std::uint64_t index = first; index <= last; ++index) {
    std::uint64_t pos = offset + done;
    std::size_t within = static_cast<std::size_t>(pos - index * cs);
    std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(data.size() - done, cs - within));
    Bytes& chunk = dirty_chunk(ino, index);
    if (chunk.size() < within + len) chunk.resize(within + len, 0);
    std::memcpy(chunk.data() + within, data.data() + done, len);
    done += len;
  }
  InodeAttrs& file = edit(ino);
  file.size = std::max(file.size, offset + data.size());
  touch(file, true);
  maybe_commit();
  return data.size();
}

void Filesystem::fallocate(InodeId ino, int mode, std::uint64_t offset, std::uint64_t length) {
  std::unique_lock lock(mutex_);
  check_writable();
  InodeAttrs before = get(ino);
  if (before.is_dir()) throw_posix(EISDIR);
  if (!before.is_regular()) throw_posix(ENODEV);
  if (length == 0) throw_posix(EINVAL);
  if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE)) throw_posix(EOPNOTSUPP);
  if (offset > kMaxFileSize || length > kMaxFileSize - offset) throw_posix(EFBIG);
  std::uint64_t end = offset + length;

  if (mode & FALLOC_FL_PUNCH_HOLE) {
    if (!(mode & FALLOC_FL_KEEP_SIZE)) throw_posix(EOPNOTSUPP);
    const std::uint64_t cs = sb_.chunk_size();
    end = std::min(end, before.size);
    for (std::uint64_t index = offset / cs; index * cs < end; ++index) {
      std::uint64_t start = index * cs;
      std::uint64_t stop = std::min(start + cs, before.size);
      if (offset <= start && stop <= end) {
        // Wholly inside the hole: forget the chunk entirely.
        if (meta_.chunk(ino, index)) record(op::RemoveChunk{ino, index});
        if (auto it = dirty_.find(ino); it != dirty_.end() && it->second.erase(index)) {
          --dirty_chunks_;
          if (it->second.empty()) dirty_.erase(it);
        }
        continue;
      }
      if (!has_data(ino, index)) continue;
      admit(cs + kDataChunkHeaderSize);
      Bytes& chunk = dirty_chunk(ino, index);
      std::uint64_t a = std::max(offset, start) - start;
      std::uint64_t b = std::min<std::uint64_t>(std::min(end, stop) - start, chunk.size());
      if (a < b) std::memset(chunk.data() + a, 0, static_cast<std::size_t>(b - a));
    }
    touch(edit(ino), true);
  } else if (!(mode & FALLOC_FL_KEEP_SIZE) && end > before.size) {
    // Nothing to preallocate in a compressed store: the new range is a hole.
    InodeAttrs& file = edit(ino);
    file.size = end;
    touch(file, true);
  }
  maybe_commit();
}

std::uint64_t Filesystem::lseek(InodeId ino, std::uint64_t offset, int whence) {
  std::shared_lock lock(mutex_);
  InodeAttrs file = get(ino);
  if (!file.is_regular()) throw_posix(EINVAL);
  if (whence != SEEK_DATA && whence != SEEK_HOLE) throw_posix(EINVAL);
  if (offset >= file.size) throw_posix(ENXIO);
  const std::uint64_t cs = sb_.chunk_size();
  const std::uint64_t first = offset / cs;
  auto dirty = dirty_.find(ino);
  if (whence == SEEK_DATA) {
    // The first chunk with data at or after `first`, stored or not yet.
    std::optional<std::uint64_t> next;
    meta_.chunks(ino, first, [&](std::uint64_t index, const ChunkRef&) {
      next = index;
      return false;
    });
    if (dirty != dirty_.end()) {
      auto it = dirty->second.lower_bound(first);
      if (it != dirty->second.end() && (!next || it->first < *next)) next = it->first;
    }
    if (!next || *next * cs >= file.size) throw_posix(ENXIO);
    return std::max(offset, *next * cs);
  }
  for (std::uint64_t index = first; index * cs < file.size; ++index) {
    if (!has_data(ino, index)) return std::max(offset, index * cs);
  }
  return file.size;  // the implicit hole at the end
}

// ---- directories -----------------------------------------------------------

std::vector<DirEntry> Filesystem::list(InodeId ino) {
  std::shared_lock lock(mutex_);
  InodeAttrs dir = get_dir(ino);
  std::vector<DirEntry> out;
  out.reserve(meta_.counts(ino).entries + 2);
  out.push_back({".", ino, S_IFDIR});
  out.push_back({"..", dir.parent, S_IFDIR});
  meta_.entries(ino, [&](std::string_view name, InodeId child) {
    out.push_back({std::string(name), child, get(child).mode & S_IFMT});
    return true;
  });
  return out;
}

// ---- extended attributes ---------------------------------------------------

Bytes Filesystem::getxattr(InodeId ino, std::string_view name) {
  std::shared_lock lock(mutex_);
  get(ino);
  if (ino == kRootInode && name == kStatsXattr) {
    std::string text = encode_stats(space_stats_locked(false));
    return Bytes(text.begin(), text.end());
  }
  std::optional<Bytes> v = meta_.xattr(ino, name);
  if (!v) throw_posix(ENODATA);
  return std::move(*v);
}

std::vector<std::string> Filesystem::listxattr(InodeId ino) {
  std::shared_lock lock(mutex_);
  get(ino);
  std::vector<std::string> names;
  meta_.xattrs(ino, [&](std::string_view name, ByteView) {
    names.emplace_back(name);
    return true;
  });
  std::sort(names.begin(), names.end());
  return names;
}

void Filesystem::setxattr(InodeId ino, std::string_view name, ByteView value, int flags) {
  std::unique_lock lock(mutex_);
  check_writable();
  get(ino);
  if (name.empty()) throw_posix(EINVAL);
  if (name.size() > kMaxXattrNameLength) throw_posix(ERANGE);
  if (value.size() > kMaxXattrValueSize) throw_posix(E2BIG);
  if (name == kPolicyXattr) {
    std::string_view v(reinterpret_cast<const char*>(value.data()), value.size());
    if (!parse_policy(v)) throw_posix(EINVAL);
  } else if (name.starts_with(kNanoerXattrPrefix)) {
    throw_posix(EPERM);  // reserved for Nanoer
  }
  bool exists = meta_.xattr(ino, name).has_value();
  if ((flags & XATTR_CREATE) && exists) throw_posix(EEXIST);
  if ((flags & XATTR_REPLACE) && !exists) throw_posix(ENODATA);
  admit(kAttrOpBytes + name.size() + value.size());
  record(op::SetXattr{ino, std::string(name), Bytes(value.begin(), value.end())});
  touch(edit(ino), false);
  maybe_commit();
}

void Filesystem::removexattr(InodeId ino, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  get(ino);
  if (!meta_.xattr(ino, name)) throw_posix(ENODATA);
  record(op::RemoveXattr{ino, std::string(name)});
  touch(edit(ino), false);
  maybe_commit();
}

// ---- statistics ------------------------------------------------------------

FsStats Filesystem::statfs() {
  std::shared_lock lock(mutex_);
  FsStats s;
  s.block_size = kBlockSize;
  s.total_blocks = sb_.segment_count * sb_.segment_size() / kBlockSize;
  std::uint64_t free_segments =
      journal_ ? journal_->free_segments() : sb_.segment_count - meta_.segments.size();
  std::uint64_t free_bytes = free_segments * sb_.segment_size() +
                             (journal_ ? journal_->current_remaining() : 0);
  s.free_blocks = free_bytes / kBlockSize;
  s.available_blocks = read_only_ ? 0 : available_bytes() / kBlockSize;
  s.files = meta_.counters.files + meta_.counters.directories + meta_.counters.other_inodes;
  s.free_files = std::uint64_t{1} << 32;
  s.name_max = kMaxNameLength;
  return s;
}

FsInfo Filesystem::info() {
  std::shared_lock lock(mutex_);
  FsInfo info;
  info.files = meta_.counters.files;
  info.directories = meta_.counters.directories;
  info.other_inodes = meta_.counters.other_inodes;
  info.logical_bytes = meta_.counters.logical_bytes;
  // Sizes changed since the last commit.
  for (const auto& [id, a] : attrs_) {
    if (!a.is_regular()) continue;
    std::optional<InodeAttrs> stored = meta_.attrs(id);
    std::uint64_t was = stored ? stored->size : 0;
    info.logical_bytes = info.logical_bytes - std::min(info.logical_bytes, was) + a.size;
  }
  for (const auto& [index, usage] : meta_.segments) info.stored_bytes += usage.live_bytes;
  info.metadata_bytes = std::min(info.stored_bytes, meta_.counters.node_bytes);
  info.stored_bytes -= info.metadata_bytes;
  info.segments_in_use = meta_.segments.size();
  info.segments_pending_free = journal_ ? journal_->pending_free_segments() : 0;
  info.segments_free = journal_ ? journal_->free_segments() : sb_.segment_count - meta_.segments.size();
  info.last_txid = meta_.last_txid;
  info.replayed_transactions = replayed_transactions_;
  info.log_bytes_since_checkpoint = journal_ ? journal_->bytes_since_checkpoint() : 0;
  info.checkpoint_bytes = checkpoint_bytes_;
  info.cache_resident = cache_.resident_bytes();
  info.cache_hits = cache_.hits();
  info.cache_misses = cache_.misses();
  info.read_only = read_only_;
  info.errors_flag = (sb_.state & state_flags::errors) != 0;
  info.errors_seen = errors_seen_;
  info.read_errors = read_errors_;
  info.log_damaged = damage_.found;
  info.checkpoint_fallback = checkpoint_fallback_;
  if (journal_) {
    info.segments_discarded = journal_->discarded_segments();
    info.discard_queued = journal_->discard_queued();
  }
  info.segment_bytes = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  info.gc = gc_stats_;
  info.tier = tier_stats_;
  info.cold_data = has_cold_data(sb_) && options_.cold_after_s >= 0 && !read_only_;
  info.dedup = dedup_ && !read_only_;
  info.dedup_entries = meta_.dedup.size();
  info.dedup_index_bytes = meta_.dedup.memory_bytes();
  info.dedup_hits = dedup_hits_;
  info.dedup_bytes = dedup_bytes_;
  return info;
}

}  // namespace nanoer
