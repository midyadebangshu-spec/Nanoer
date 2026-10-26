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
      cache_(options.safe_space) {
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
  auto careful = [&](const Superblock& from) {
    try {
      return replay(store, from);
    } catch (const ReplayError& e) {
      std::fprintf(stderr, "nanoer: the log contradicts itself (%s)\n", e.what());
      contradiction = true;
      return replay(store, from, e.txid());
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
  std::unique_ptr<Filesystem> fs(new Filesystem(store, sb, std::move(*replayed), options, read_only));
  fs->damage_ = damage;
  fs->checkpoint_fallback_ = fell_back;
  if (fell_back || damage.found) fs->errors_seen_ = true;

  if (!fs->read_only_) {
    std::unique_lock lock(fs->mutex_);
    fs->journal_ = std::make_unique<Journal>(store, fs->sb_, fs->meta_, next_seq);
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
    fs->journal_->resume(end);
    fs->journal_->set_reserve(kMinReserveSegments + kGcHeadroomSegments, kGcHeadroomSegments);
    // Files deleted while still open when the system went down.
    std::vector<InodeId> orphans;
    for (const auto& [id, inode] : fs->meta_.inodes) {
      if (inode.nlink == 0 && id != kRootInode) orphans.push_back(id);
    }
    for (InodeId id : orphans) fs->maybe_free(id);
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
  std::vector<InodeId> orphans;
  for (const auto& [id, inode] : meta_.inodes) {
    if (inode.nlink == 0 && id != kRootInode) orphans.push_back(id);
  }
  for (InodeId id : orphans) maybe_free(id);
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

Inode& Filesystem::inode(InodeId ino) {
  Inode* i = meta_.find(ino);
  if (!i) throw_posix(ENOENT);
  return *i;
}

Inode& Filesystem::directory(InodeId ino) {
  Inode& d = inode(ino);
  if (!d.is_dir()) throw_posix(ENOTDIR);
  return d;
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

void Filesystem::mark(InodeId ino) {
  last_change_ns_ = now();
  if (dirty_inodes_.insert(ino).second) pending_bytes_ += kAttrOpBytes;
}

void Filesystem::touch(Inode& i, bool content) {
  std::int64_t t = now();
  i.ctime_ns = t;
  if (content) i.mtime_ns = t;
  mark(i.id);
}

void Filesystem::count_lookup(InodeId ino) {
  if (!options_.count_lookups) return;
  std::lock_guard lock(counts_mutex_);
  ++lookups_[ino];
}

void Filesystem::maybe_free(InodeId ino) {
  if (read_only_ || ino == kRootInode) return;
  Inode* i = meta_.find(ino);
  if (!i || i->nlink != 0) return;
  {
    std::lock_guard lock(counts_mutex_);
    if (lookups_.contains(ino) || opens_.contains(ino)) return;
  }
  drop_dirty(ino, 0);
  dirty_inodes_.erase(ino);
  record(op::DeleteInode{ino});
}

bool Filesystem::is_ancestor(InodeId ancestor, InodeId dir) const {
  for (InodeId at = dir;;) {
    if (at == ancestor) return true;
    if (at == kRootInode) return false;
    const Inode* i = meta_.find(at);
    if (!i) return false;
    at = i->parent;
  }
}

Attr Filesystem::make_attr(const Inode& i) const {
  Attr a;
  a.ino = i.id;
  a.mode = i.mode;
  a.nlink = i.nlink;
  a.uid = i.uid;
  a.gid = i.gid;
  a.size = i.is_dir() ? i.entries.size() : i.size;
  a.rdev = i.rdev;
  a.blksize = sb_.chunk_size();
  a.atime_ns = i.atime_ns;
  a.mtime_ns = i.mtime_ns;
  a.ctime_ns = i.ctime_ns;
  a.btime_ns = i.btime_ns;
  if (i.is_regular()) {
    std::uint64_t bytes = 0;
    for (const auto& [index, ref] : i.chunks) bytes += ref.size;
    if (auto it = dirty_.find(i.id); it != dirty_.end()) {
      for (const auto& [index, data] : it->second) {
        if (!i.chunks.contains(index)) bytes += data.size();
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
  std::shared_ptr<const Bytes> data = cache_.get(key);
  if (!data) {
    Record record = read_record(store_, ref.offset, sb_.segment_offset(segment) + sb_.segment_size(),
                                it->second.seq);
    data = std::make_shared<const Bytes>(decode_data_chunk(record));
    if (!ref.packed && data->size() != ref.size) throw Error(Errc::corrupt, "chunk has the wrong size");
    cache_.put(key, data);  // a pack whole: its pieces are often read together
  }
  if (!ref.packed) return data;
  if (std::uint64_t{ref.piece} + ref.size > data->size()) throw Error(Errc::corrupt, "piece outside its pack");
  return std::make_shared<const Bytes>(data->begin() + ref.piece, data->begin() + ref.piece + ref.size);
}

Bytes Filesystem::load_chunk(const Inode& file, std::uint64_t index) {
  auto it = file.chunks.find(index);
  if (it == file.chunks.end()) return {};
  return *read_chunk(it->second);
}

Bytes& Filesystem::dirty_chunk(Inode& file, std::uint64_t index) {
  auto& chunks = dirty_[file.id];
  auto it = chunks.find(index);
  if (it == chunks.end()) {
    it = chunks.emplace(index, load_chunk(file, index)).first;
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

void Filesystem::truncate(Inode& file, std::uint64_t size) {
  if (size > kMaxFileSize) throw_posix(EFBIG);
  if (size == file.size) return;
  const std::uint64_t cs = sb_.chunk_size();
  if (size < file.size) {
    std::uint64_t first_gone = (size + cs - 1) / cs;
    if (file.chunks.lower_bound(first_gone) != file.chunks.end()) {
      record(op::TruncateChunks{file.id, first_gone});
    }
    drop_dirty(file.id, first_gone);
    // The chunk the new end falls in keeps bytes past it; rewrite it trimmed,
    // so they cannot reappear if the file grows again.
    if (size % cs != 0) {
      std::uint64_t last = size / cs;
      bool has_data = file.chunks.contains(last) ||
                      (dirty_.contains(file.id) && dirty_[file.id].contains(last));
      if (has_data) {
        Bytes& data = dirty_chunk(file, last);
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
  if (journal_->pending_free_segments() > 0) return true;
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
  if (dirty_.empty() && pending_ops_.empty() && dirty_inodes_.empty()) return;
  try {
    struct Job {
      InodeId ino;
      std::uint64_t index;
      Bytes* data;
      bool hole;
      EncodedChunk encoded;
      Blake3Hash id{};
      std::optional<ChunkRef> duplicate;  // of live data
      std::optional<std::size_t> same_as;  // an earlier job in this commit
      bool packed = false;                 // stored as a piece of a pack
      ChunkRef ref;
    };
    const std::uint64_t cs = sb_.chunk_size();
    std::vector<Job> jobs;
    for (auto& [ino, chunks] : dirty_) {
      const Inode* file = meta_.find(ino);
      if (!file) continue;
      for (auto& [index, data] : chunks) {
        std::uint64_t start = index * cs;
        if (start >= file->size) continue;
        // The file may have grown past this chunk since it was written:
        // the bytes in between are zeros.
        data.resize(static_cast<std::size_t>(std::min(cs, file->size - start)), 0);
        jobs.push_back({ino, index, &data, all_zero(data), {}});
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
        job.duplicate = find_duplicate(job.id, *job.data);
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
    };
    std::vector<Pack> packs;
    if (has_shared_records(sb_)) {
      const std::uint64_t limit = std::min(kPackFileLimit, cs);
      std::vector<std::size_t> small;
      std::unordered_set<InodeId> small_inodes;
      for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Job& job = jobs[i];
        if (job.hole || job.duplicate || job.same_as || job.index != 0) continue;
        const Inode* file = meta_.find(job.ino);
        if (file->size > limit || file->size != job.data->size()) continue;
        small.push_back(i);
        small_inodes.insert(job.ino);
      }
      if (small.size() >= 2) {
        std::unordered_map<InodeId, std::pair<InodeId, std::string_view>> where;
        for (InodeId d : dirty_inodes_) {
          const Inode* dir = meta_.find(d);
          if (!dir || !dir->is_dir()) continue;
          for (const auto& [name, child] : dir->entries) {
            if (small_inodes.contains(child)) where.try_emplace(child, d, name);
          }
        }
        auto order = [&](std::size_t i) {
          auto w = where.find(jobs[i].ino);
          return w == where.end() ? std::make_tuple(1, jobs[i].ino, std::string_view(), jobs[i].ino)
                                  : std::make_tuple(0, w->second.first, w->second.second, jobs[i].ino);
        };
        std::sort(small.begin(), small.end(), [&](std::size_t a, std::size_t b) { return order(a) < order(b); });
        for (std::size_t i : small) {
          if (packs.empty() || packs.back().data.size() + jobs[i].data->size() > kPackSize) packs.emplace_back();
          Pack& p = packs.back();
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
        job.encoded = encode_chunk(*job.data, sb_.default_codec, sb_.default_level);
      }
    });
    parallel(packs.size(), [&](std::size_t i) {
      packs[i].encoded = encode_chunk(packs[i].data, sb_.default_codec, sb_.default_level);
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
          job.encoded = encode_chunk(*job.data, sb_.default_codec, sb_.default_level);
        }
      }
      if (job.hole) {
        if (!meta_.find(job.ino)->chunks.contains(job.index)) continue;
        op = op::RemoveChunk{job.ino, job.index};  // all zeros: store nothing
      } else if (job.duplicate || job.same_as) {
        job.ref = job.duplicate ? *job.duplicate : jobs[*job.same_as].ref;
        op = op::SetChunkRef{job.ino, job.index, job.ref, !job.ref.packed};
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
    for (InodeId ino : dirty_inodes_) {
      if (const Inode* i = meta_.find(ino)) ops.push_back(op::PutInode{*i});
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
  dirty_inodes_.clear();
  pending_bytes_ = 0;
  last_commit_ns_ = now();
  maybe_checkpoint();
}

std::optional<ChunkRef> Filesystem::find_duplicate(const Blake3Hash& id, ByteView data) {
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
  try {
    DataChunkInfo info = parse_data_chunk(*r);
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
  return ref;
}

void Filesystem::maybe_checkpoint() {
  if (!journal_ || in_checkpoint_) return;
  std::uint64_t written = journal_->bytes_since_checkpoint();
  bool overdue = written > 0 && now() - last_checkpoint_ns_ >=
                                    std::int64_t{options_.checkpoint_interval_s} * 1'000'000'000;
  if (written >= options_.checkpoint_bytes || overdue) checkpoint();
}

void Filesystem::checkpoint() {
  if (read_only_ || failed_ || !journal_ || in_checkpoint_) return;
  in_checkpoint_ = true;
  try {
    commit();
    LogPosition at = journal_->append_checkpoint(meta_);
    journal_->flush();
    sb_.checkpoint_offset = at.offset;
    sb_.checkpoint_seq = at.seq;
    sb_.next_segment_seq = journal_->next_seq();
    if (errors_seen_) sb_.state |= state_flags::errors;
    write_superblock(store_, sb_);
    journal_->checkpoint_done(at);
    // Keep room for the next checkpoint (it grows with the metadata).
    std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
    checkpoint_bytes_ = meta_.encode_checkpoint().size();
    journal_->set_reserve(kMinReserveSegments + kGcHeadroomSegments + (checkpoint_bytes_ * 3 / 2 + usable - 1) / usable,
                          kGcHeadroomSegments);
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
  inode(ino);
  if (read_only_) return;
  if (failed_) throw_posix(EIO);
  commit();  // commits everything before it, not just this file: simple and stronger
}

// ---- attributes and lookups ------------------------------------------------

Attr Filesystem::getattr(InodeId ino) {
  std::shared_lock lock(mutex_);
  return make_attr(inode(ino));
}

Attr Filesystem::lookup(InodeId parent, std::string_view name) {
  std::shared_lock lock(mutex_);
  Inode& dir = directory(parent);
  if (name.size() > kMaxNameLength) throw_posix(ENAMETOOLONG);
  InodeId ino;
  if (name == ".") {
    ino = parent;
  } else if (name == "..") {
    ino = dir.parent;
  } else {
    auto it = dir.entries.find(name);
    if (it == dir.entries.end()) throw_posix(ENOENT);
    ino = it->second;
  }
  Attr a = make_attr(inode(ino));
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
  Inode& i = inode(ino);
  // Shrinking a file frees space, so it must work on a full filesystem too.
  if (!r.size || *r.size >= i.size) admit(kAttrOpBytes);
  if (r.size) {
    if (i.is_dir()) throw_posix(EISDIR);
    if (!i.is_regular()) throw_posix(EINVAL);
    truncate(i, *r.size);
  }
  if (r.mode) i.mode = (i.mode & S_IFMT) | (*r.mode & 07777);
  if (r.uid) i.uid = *r.uid;
  if (r.gid) i.gid = *r.gid;
  std::int64_t t = now();
  if (r.atime == SetAttrRequest::Time::now) i.atime_ns = t;
  if (r.atime == SetAttrRequest::Time::set) i.atime_ns = r.atime_ns;
  if (r.mtime == SetAttrRequest::Time::now) i.mtime_ns = t;
  if (r.mtime == SetAttrRequest::Time::set) i.mtime_ns = r.mtime_ns;
  i.ctime_ns = t;
  mark(ino);
  Attr a = make_attr(i);
  maybe_commit();
  return a;
}

std::string Filesystem::readlink(InodeId ino) {
  std::shared_lock lock(mutex_);
  Inode& i = inode(ino);
  if (!i.is_symlink()) throw_posix(EINVAL);
  return i.symlink;
}

// ---- creating and removing -------------------------------------------------

Attr Filesystem::create(InodeId parent, std::string_view name, std::uint32_t mode,
                        std::uint64_t rdev, std::string_view target, const Caller& caller) {
  check_writable();
  Inode& dir = directory(parent);
  check_name(name);
  if (dir.entries.contains(name)) throw_posix(EEXIST);
  bool is_dir = S_ISDIR(mode);
  if (is_dir && dir.nlink >= kMaxLinks) throw_posix(EMLINK);
  admit(kAttrOpBytes * 2 + name.size() + target.size());

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
  record(op::SetEntry{parent, std::string(name), a.id});
  if (is_dir) ++dir.nlink;
  touch(dir, true);
  count_lookup(a.id);
  Attr result = make_attr(inode(a.id));
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
  Inode& i = inode(ino);
  if (i.is_dir()) throw_posix(EPERM);
  Inode& dir = directory(new_parent);
  check_name(new_name);
  if (dir.entries.contains(new_name)) throw_posix(EEXIST);
  if (i.nlink >= kMaxLinks) throw_posix(EMLINK);
  admit(kAttrOpBytes * 2 + new_name.size());
  record(op::SetEntry{new_parent, std::string(new_name), ino});
  ++i.nlink;
  touch(i, false);
  touch(dir, true);
  count_lookup(ino);
  Attr a = make_attr(i);
  maybe_commit();
  return a;
}

void Filesystem::unlink(InodeId parent, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  Inode& dir = directory(parent);
  auto it = dir.entries.find(name);
  if (it == dir.entries.end()) throw_posix(ENOENT);
  Inode& target = inode(it->second);
  if (target.is_dir()) throw_posix(EISDIR);
  // No space check: deleting is how a full filesystem gets space back. The
  // transaction is tiny and may use the reserve.
  record(op::RemoveEntry{parent, std::string(name)});
  --target.nlink;
  touch(target, false);
  touch(dir, true);
  maybe_free(target.id);
  maybe_commit();
}

void Filesystem::rmdir(InodeId parent, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  Inode& dir = directory(parent);
  if (name == ".") throw_posix(EINVAL);
  if (name == "..") throw_posix(ENOTEMPTY);
  auto it = dir.entries.find(name);
  if (it == dir.entries.end()) throw_posix(ENOENT);
  Inode& target = inode(it->second);
  if (!target.is_dir()) throw_posix(ENOTDIR);
  if (!target.entries.empty()) throw_posix(ENOTEMPTY);
  record(op::RemoveEntry{parent, std::string(name)});  // no space check, as for unlink
  target.nlink = 0;
  --dir.nlink;
  touch(target, false);
  touch(dir, true);
  maybe_free(target.id);
  maybe_commit();
}

void Filesystem::rename(InodeId parent, std::string_view name, InodeId new_parent,
                        std::string_view new_name, unsigned flags) {
  std::unique_lock lock(mutex_);
  check_writable();
  if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) throw_posix(EINVAL);
  if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) throw_posix(EINVAL);
  Inode& src_dir = directory(parent);
  Inode& dst_dir = directory(new_parent);
  check_name(new_name);
  auto src_it = src_dir.entries.find(name);
  if (src_it == src_dir.entries.end()) throw_posix(ENOENT);
  InodeId src = src_it->second;
  auto dst_it = dst_dir.entries.find(new_name);
  InodeId dst = dst_it == dst_dir.entries.end() ? 0 : dst_it->second;
  if (parent == new_parent && name == new_name) return;
  Inode& s = inode(src);
  admit(kAttrOpBytes * 4 + name.size() + new_name.size());

  if (flags & RENAME_EXCHANGE) {
    if (!dst) throw_posix(ENOENT);
    Inode& d = inode(dst);
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
    Inode& d = inode(dst);
    if (s.is_dir() && !d.is_dir()) throw_posix(ENOTDIR);
    if (!s.is_dir() && d.is_dir()) throw_posix(EISDIR);
    if (d.is_dir() && !d.entries.empty()) throw_posix(ENOTEMPTY);
  }

  record(op::SetEntry{new_parent, std::string(new_name), src});
  record(op::RemoveEntry{parent, std::string(name)});
  if (dst) {
    Inode& d = inode(dst);
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
  inode(ino);
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
  Inode& file = inode(ino);
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
      if (auto c = file.chunks.find(index); c != file.chunks.end()) {
        try {
          stored = read_chunk(c->second);
        } catch (const Error& e) {
          if (e.code() != Errc::corrupt) throw;
          // Report exactly what is lost; never return damaged data.
          ++read_errors_;
          errors_seen_ = true;
          std::fprintf(stderr, "nanoer: damaged data in inode %llu, bytes %llu-%llu: %s\n",
                       static_cast<unsigned long long>(ino),
                       static_cast<unsigned long long>(index * cs),
                       static_cast<unsigned long long>(index * cs + c->second.size - 1), e.what());
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
  Inode& file = inode(ino);
  if (file.is_dir()) throw_posix(EISDIR);
  if (!file.is_regular()) throw_posix(EINVAL);
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
    Bytes& chunk = dirty_chunk(file, index);
    if (chunk.size() < within + len) chunk.resize(within + len, 0);
    std::memcpy(chunk.data() + within, data.data() + done, len);
    done += len;
  }
  file.size = std::max(file.size, offset + data.size());
  touch(file, true);
  maybe_commit();
  return data.size();
}

void Filesystem::fallocate(InodeId ino, int mode, std::uint64_t offset, std::uint64_t length) {
  std::unique_lock lock(mutex_);
  check_writable();
  Inode& file = inode(ino);
  if (file.is_dir()) throw_posix(EISDIR);
  if (!file.is_regular()) throw_posix(ENODEV);
  if (length == 0) throw_posix(EINVAL);
  if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE)) throw_posix(EOPNOTSUPP);
  if (offset > kMaxFileSize || length > kMaxFileSize - offset) throw_posix(EFBIG);
  std::uint64_t end = offset + length;

  if (mode & FALLOC_FL_PUNCH_HOLE) {
    if (!(mode & FALLOC_FL_KEEP_SIZE)) throw_posix(EOPNOTSUPP);
    const std::uint64_t cs = sb_.chunk_size();
    end = std::min(end, file.size);
    for (std::uint64_t index = offset / cs; index * cs < end; ++index) {
      std::uint64_t start = index * cs;
      std::uint64_t stop = std::min(start + cs, file.size);
      if (offset <= start && stop <= end) {
        // Wholly inside the hole: forget the chunk entirely.
        if (file.chunks.contains(index)) record(op::RemoveChunk{ino, index});
        if (auto it = dirty_.find(ino); it != dirty_.end() && it->second.erase(index)) {
          --dirty_chunks_;
          if (it->second.empty()) dirty_.erase(it);
        }
        continue;
      }
      bool has_data =
          file.chunks.contains(index) || (dirty_.contains(ino) && dirty_[ino].contains(index));
      if (!has_data) continue;
      admit(cs + kDataChunkHeaderSize);
      Bytes& chunk = dirty_chunk(file, index);
      std::uint64_t a = std::max(offset, start) - start;
      std::uint64_t b = std::min<std::uint64_t>(std::min(end, stop) - start, chunk.size());
      if (a < b) std::memset(chunk.data() + a, 0, static_cast<std::size_t>(b - a));
    }
    touch(file, true);
  } else if (!(mode & FALLOC_FL_KEEP_SIZE) && end > file.size) {
    // Nothing to preallocate in a compressed store: the new range is a hole.
    file.size = end;
    touch(file, true);
  }
  maybe_commit();
}

std::uint64_t Filesystem::lseek(InodeId ino, std::uint64_t offset, int whence) {
  std::shared_lock lock(mutex_);
  Inode& file = inode(ino);
  if (!file.is_regular()) throw_posix(EINVAL);
  if (whence != SEEK_DATA && whence != SEEK_HOLE) throw_posix(EINVAL);
  if (offset >= file.size) throw_posix(ENXIO);
  const std::uint64_t cs = sb_.chunk_size();
  auto dirty = dirty_.find(ino);
  auto has_data = [&](std::uint64_t index) {
    return file.chunks.contains(index) ||
           (dirty != dirty_.end() && dirty->second.contains(index));
  };
  for (std::uint64_t index = offset / cs; index * cs < file.size; ++index) {
    if (has_data(index) == (whence == SEEK_DATA)) return std::max(offset, index * cs);
  }
  if (whence == SEEK_HOLE) return file.size;  // the implicit hole at the end
  throw_posix(ENXIO);
}

// ---- directories -----------------------------------------------------------

std::vector<DirEntry> Filesystem::list(InodeId ino) {
  std::shared_lock lock(mutex_);
  Inode& dir = directory(ino);
  std::vector<DirEntry> out;
  out.reserve(dir.entries.size() + 2);
  out.push_back({".", ino, S_IFDIR});
  out.push_back({"..", dir.parent, S_IFDIR});
  for (const auto& [name, child] : dir.entries) {
    out.push_back({name, child, inode(child).mode & S_IFMT});
  }
  return out;
}

// ---- extended attributes ---------------------------------------------------

Bytes Filesystem::getxattr(InodeId ino, std::string_view name) {
  std::shared_lock lock(mutex_);
  Inode& i = inode(ino);
  auto it = i.xattrs.find(name);
  if (it == i.xattrs.end()) throw_posix(ENODATA);
  return it->second;
}

std::vector<std::string> Filesystem::listxattr(InodeId ino) {
  std::shared_lock lock(mutex_);
  std::vector<std::string> names;
  for (const auto& [name, value] : inode(ino).xattrs) names.push_back(name);
  return names;
}

void Filesystem::setxattr(InodeId ino, std::string_view name, ByteView value, int flags) {
  std::unique_lock lock(mutex_);
  check_writable();
  Inode& i = inode(ino);
  if (name.empty()) throw_posix(EINVAL);
  if (name.size() > kMaxXattrNameLength) throw_posix(ERANGE);
  if (value.size() > kMaxXattrValueSize) throw_posix(E2BIG);
  bool exists = i.xattrs.contains(name);
  if ((flags & XATTR_CREATE) && exists) throw_posix(EEXIST);
  if ((flags & XATTR_REPLACE) && !exists) throw_posix(ENODATA);
  admit(kAttrOpBytes + name.size() + value.size());
  record(op::SetXattr{ino, std::string(name), Bytes(value.begin(), value.end())});
  touch(i, false);
  maybe_commit();
}

void Filesystem::removexattr(InodeId ino, std::string_view name) {
  std::unique_lock lock(mutex_);
  check_writable();
  Inode& i = inode(ino);
  if (!i.xattrs.contains(name)) throw_posix(ENODATA);
  record(op::RemoveXattr{ino, std::string(name)});
  touch(i, false);
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
  s.files = meta_.inodes.size();
  s.free_files = std::uint64_t{1} << 32;
  s.name_max = kMaxNameLength;
  return s;
}

FsInfo Filesystem::info() {
  std::shared_lock lock(mutex_);
  FsInfo info;
  for (const auto& [id, i] : meta_.inodes) {
    if (i.is_regular()) {
      ++info.files;
      info.logical_bytes += i.size;
    } else if (i.is_dir()) {
      ++info.directories;
    } else {
      ++info.other_inodes;
    }
  }
  for (const auto& [index, usage] : meta_.segments) info.stored_bytes += usage.live_bytes;
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
  info.segment_bytes = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  info.gc = gc_stats_;
  info.dedup = dedup_ && !read_only_;
  info.dedup_entries = meta_.dedup.size();
  info.dedup_index_bytes = meta_.dedup.memory_bytes();
  info.dedup_hits = dedup_hits_;
  info.dedup_bytes = dedup_bytes_;
  return info;
}

}  // namespace nanoer
