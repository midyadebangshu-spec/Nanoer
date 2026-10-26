#include "nanoer/check.h"

#include <algorithm>
#include <bit>
#include <map>
#include <deque>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <sys/stat.h>

#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/io.h"
#include "nanoer/journal.h"
#include "nanoer/metadata.h"
#include "nanoer/record.h"
#include "nanoer/segment.h"

namespace nanoer {

int FsckReport::exit_code() const {
  if (!errors.empty()) return 4;
  if (!fixed.empty()) return 1;
  return 0;
}

namespace {

std::string range(std::uint64_t first, std::uint64_t length) {
  return "bytes " + std::to_string(first) + "-" + std::to_string(first + length - 1);
}

const char* kind(const Inode& i) {
  if (i.is_dir()) return "directory";
  if (i.is_regular()) return "file";
  if (i.is_symlink()) return "symlink";
  return "special file";
}

// ---- superblock rescue -----------------------------------------------------

// No superblock leads to a readable checkpoint, but segments and
// checkpoints are still there. Find the geometry (from `known`, a superblock
// that is still valid, or else from the segment headers), and the newest
// checkpoint that replays cleanly; then write new superblocks pointing at it.
void rescue_superblocks(Store& store, FsckReport& report, const std::optional<Superblock>& known) {
  const std::uint64_t data_start = known ? known->data_start : kDataStart;  // every mkfs uses it
  if (store.size() < data_start + (std::uint64_t{1} << kMinSegmentShift)) {
    throw Error(Errc::corrupt, "no Nanoer filesystem found (store too small)");
  }
  // The segment size: the shift at which most headers line up with their index.
  unsigned best_shift = known ? known->segment_shift : 0;
  std::size_t best = known ? 1 : 0;
  Uuid uuid = known ? known->uuid : Uuid{};
  for (unsigned shift = kMinSegmentShift; !known && shift <= kMaxSegmentShift; ++shift) {
    std::uint64_t count = (store.size() - data_start) >> shift;
    if (count < 1) break;
    std::map<Uuid, std::size_t> votes;
    for (std::uint64_t i = 1; i < std::min<std::uint64_t>(count, 4096); ++i) {
      Bytes block = store.read(data_start + (i << shift), kSegmentHeaderSize);
      // Accept any filesystem's header here; the UUID is decided by vote.
      Uuid u;
      std::copy_n(block.begin() + 8, u.size(), u.begin());
      if (decode_segment_header(block, u, i)) ++votes[u];
    }
    for (const auto& [u, n] : votes) {
      if (n > best) {
        best = n;
        best_shift = shift;
        uuid = u;
      }
    }
  }
  if (best == 0) {
    // Only segment 0 was ever used: its header names the filesystem.
    Bytes block = store.read(data_start, kSegmentHeaderSize);
    std::copy_n(block.begin() + 8, uuid.size(), uuid.begin());
    if (!decode_segment_header(block, uuid, 0)) {
      throw Error(Errc::corrupt, "no Nanoer filesystem found (no superblock, no segment headers)");
    }
    best_shift = kDefaultSegmentShift;
    while (best_shift > kMinSegmentShift &&
           (store.size() - data_start) >> best_shift < kMinSegmentCount) {
      --best_shift;
    }
  }

  Superblock sb = known ? *known : Superblock{};
  sb.uuid = uuid;
  sb.store_size = store.size();
  sb.data_start = data_start;
  sb.segment_shift = best_shift;
  sb.segment_count = (store.size() - data_start) >> best_shift;
  sb.created_ns = sb.updated_ns = now_ns();

  // Every checkpoint's first part, newest first.
  struct Candidate {
    std::uint64_t txid, offset, seq;
    bool shared_records;  // its snapshot says which encoding, so which feature
  };
  std::vector<Candidate> candidates;
  std::uint64_t max_seq = 0;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    SegmentScanner scan(store, sb, i);
    if (!scan.valid()) continue;
    max_seq = std::max(max_seq, scan.header().seq);
    while (auto r = scan.next()) {
      if (r->header.type == RecordType::checkpoint && r->extra.size() == kCheckpointExtraSize &&
          load_le<std::uint32_t>(r->extra.data() + 8) == 0) {
        candidates.push_back({load_le<std::uint64_t>(r->extra.data()), r->offset, scan.header().seq,
                              Metadata::is_v2_checkpoint(r->payload)});
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.txid > b.txid; });
  for (const Candidate& c : candidates) {
    sb.checkpoint_offset = c.offset;
    sb.checkpoint_seq = c.seq;
    sb.next_segment_seq = max_seq + 1;
    if (!known) {
      sb.features_incompat = c.shared_records ? incompat_features::shared_records : 0;
    }
    try {
      ReplayResult r = replay(store, sb);
      // The chunk size: any chunk past the first tells it exactly.
      std::uint64_t chunk = 0, largest = 0;
      for (const auto& [id, inode] : r.meta.inodes) {
        for (const auto& [index, ref] : inode.chunks) {
          largest = std::max<std::uint64_t>(largest, ref.size);
          if (index == 0 || chunk) continue;
          std::uint64_t seg = r.meta.segment_of(ref.offset);
          auto usage = r.meta.segments.find(seg);
          if (usage == r.meta.segments.end()) continue;
          auto h = try_read_record_header(store, ref.offset, sb.segment_offset(seg) + sb.segment_size(),
                                          usage->second.seq);
          if (h && h->header.type == RecordType::data_chunk) {
            chunk = parse_data_chunk(*h).file_offset / index;
          }
        }
      }
      if (!known) {
        if (!chunk) chunk = std::max<std::uint64_t>(std::uint64_t{1} << kDefaultChunkShift, std::bit_ceil(largest));
        sb.chunk_shift = static_cast<std::uint8_t>(std::countr_zero(chunk));
        if (sb.chunk_shift >= sb.segment_shift) sb.chunk_shift = static_cast<std::uint8_t>(sb.segment_shift - 1);
      }
      sb.state |= state_flags::errors;  // cleared once the rest of the check passes
      write_superblock(store, sb);
      write_superblock(store, sb);
      report.fixed.push_back(
          std::string(known ? "no superblock pointed at a readable checkpoint"
                            : "both superblocks were unreadable") +
          "; rebuilt them from the checkpoint of transaction " + std::to_string(c.txid) +
          (known ? "" : " (the label and the default compression settings could not be recovered)"));
      return;
    } catch (const Error&) {
      // Try the next older checkpoint.
    }
  }
  throw Error(Errc::corrupt, std::string(known ? "no checkpoint" : "no superblock, and no checkpoint") +
                                 " that can be read");
}

// ---- the check -------------------------------------------------------------

class Checker {
 public:
  Checker(Store& store, const FsckOptions& options, FsckReport& report)
      : store_(store), options_(options), report_(report) {}

  void run() {
    superblocks();
    if (!loaded_) return;
    log();
    if (!meta_) return;
    tree();
    space();
    if (options_.repair) repair();
  }

 private:
  void error(const std::string& message) { report_.errors.push_back(message); }
  void note(const std::string& message) { report_.notes.push_back(message); }

  // Records a repair: applied to the metadata at once, so later checks see
  // the repaired state, and written out by repair().
  void fix(Op op) {
    if (!options_.repair) return;
    meta_->apply(op);
    fixes_.push_back(std::move(op));
  }

  std::string path_of(InodeId ino) const {
    auto it = paths_.find(ino);
    return it != paths_.end() ? it->second : "inode " + std::to_string(ino);
  }

  // Phase 1.
  void superblocks() {
    if (store_.size() < kSuperblockOffsets[1] + kBlockSize) {
      throw Error(Errc::corrupt, "the store is too small to hold a Nanoer filesystem");
    }
    DecodedSuperblock slots[2];
    for (int i = 0; i < 2; ++i) slots[i] = decode_superblock(store_.read(kSuperblockOffsets[i], kBlockSize));
    try {
      loaded_ = load_superblock(store_);
    } catch (const Error& e) {
      if (e.code() == Errc::unsupported) throw;
      if (!options_.repair) {
        error(std::string("no valid superblock (") + e.what() +
              "); run with --repair to rebuild it from the newest checkpoint");
        return;
      }
      rescue_superblocks(store_, report_, std::nullopt);
      loaded_ = load_superblock(store_);
      rewrite_both_ = true;
    }
    if (const DecodedSuperblock& other = slots[1 - loaded_->slot];
        other.state != DecodedSuperblock::State::valid && !rewrite_both_) {
      // A superblock write cut short by a crash leaves exactly this.
      note("superblock slot " + std::to_string(1 - loaded_->slot) + " is not valid (" + other.reason +
           "); the next checkpoint rewrites it");
      rewrite_both_ = options_.repair;
    }
    sb_ = loaded_->sb;
    if (sb_.state & state_flags::errors) {
      note("the filesystem recorded earlier that it met damage");
      clear_errors_flag_ = true;
    }
  }

  // Phase 2.
  void log() {
    std::map<std::uint64_t, int> seqs;
    for (std::uint64_t i = 0; i < sb_.segment_count; ++i) {
      if (auto h = read_segment_header(store_, sb_, i)) ++seqs[h->seq];
    }
    for (const auto& [seq, n] : seqs) {
      if (n > 1) note("sequence number " + std::to_string(seq) + " is in " + std::to_string(n) +
                      " segment headers (left over from crashes; harmless)");
    }
    try {
      replayed_ = replay_carefully(sb_);
    } catch (const CheckpointError& e) {
      error(std::string("the newest checkpoint is damaged: ") + e.what());
      try {
        if (!loaded_->older) throw CheckpointError("there is no older checkpoint");
        replayed_ = replay_carefully(*loaded_->older);
        sb_ = *loaded_->older;
      } catch (const CheckpointError& older) {
        // Last resort: the newest checkpoint that can still be read, found
        // by scanning the segments.
        if (!options_.repair) {
          error(std::string("the previous checkpoint cannot be used either (") + older.what() +
                "); run with --repair to search the segments for the newest readable checkpoint");
          return;
        }
        rescue_superblocks(store_, report_, sb_);
        loaded_ = load_superblock(store_);
        sb_ = loaded_->sb;
        replayed_ = replay_carefully(sb_);
        rewrite_both_ = true;
      }
      new_checkpoint_ = true;
    }
    if (replayed_->damage.found) {
      error("the log is damaged at byte " + std::to_string(replayed_->damage.offset) +
            ": transactions after " + std::to_string(replayed_->meta.last_txid) +
            " (up to at least " + std::to_string(replayed_->damage.durable_claim) +
            ") cannot be read");
      report_.lost.push_back("every change after transaction " +
                             std::to_string(replayed_->meta.last_txid));
      new_checkpoint_ = true;
    }
    meta_ = replayed_->meta;
  }

  ReplayResult replay_carefully(const Superblock& sb) {
    try {
      return replay(store_, sb);
    } catch (const ReplayError& e) {
      error(std::string("the log contradicts itself at ") + e.what());
      report_.lost.push_back("every change from transaction " + std::to_string(e.txid()) + " on");
      new_checkpoint_ = true;
      return replay(store_, sb, e.txid());
    }
  }

  // Phase 3.
  void tree() {
    Inode* root = meta_->find(kRootInode);
    paths_[kRootInode] = "/";
    if (root->parent != kRootInode) {
      error("the root directory's parent is wrong");
      InodeAttrs a = *root;
      a.parent = kRootInode;
      fix(op::PutInode{a});
    }

    // Walk from the root: every directory reached once, by one name.
    std::unordered_set<InodeId> reached = {kRootInode};
    std::deque<InodeId> queue = {kRootInode};
    while (!queue.empty()) {
      InodeId d = queue.front();
      queue.pop_front();
      std::vector<std::pair<std::string, InodeId>> entries(meta_->find(d)->entries.begin(),
                                                           meta_->find(d)->entries.end());
      for (const auto& [name, child] : entries) {
        std::string path = (d == kRootInode ? "/" : paths_[d] + "/") + name;
        Inode* c = meta_->find(child);
        if (!c) {
          error(path + " points at inode " + std::to_string(child) + ", which does not exist");
          fix(op::RemoveEntry{d, name});
          continue;
        }
        if (c->is_dir()) {
          if (reached.contains(child)) {
            error("directory " + path_of(child) + " also appears as " + path + " (a directory may "
                  "have only one name)");
            fix(op::RemoveEntry{d, name});
            continue;
          }
          if (c->parent != d) {
            error("directory " + path + " has the wrong parent");
            InodeAttrs a = *c;
            a.parent = d;
            fix(op::PutInode{a});
          }
          queue.push_back(child);
        }
        reached.insert(child);
        paths_.try_emplace(child, path);
      }
    }

    // Orphans: deleted while still open when the system went down.
    std::uint64_t orphans = 0;
    std::vector<InodeId> doomed;
    for (const auto& [id, inode] : meta_->inodes) {
      if (!reached.contains(id) && inode.nlink == 0) doomed.push_back(id);
    }
    for (InodeId id : doomed) {
      ++orphans;
      fix(op::DeleteInode{id});
    }

    // Lost inodes: still linked, but not reachable from the root. Only the
    // top of each lost subtree is reconnected; what is under it comes along.
    std::unordered_set<InodeId> reported;
    for (int round = 0; round < 1000; ++round) {
      std::unordered_set<InodeId> reachable = reachable_from_root();
      std::unordered_set<InodeId> named;  // named by some directory (reachable or not)
      std::vector<InodeId> lost;
      for (const auto& [id, inode] : meta_->inodes) {
        for (const auto& [name, child] : inode.entries) named.insert(child);
        if (!reachable.contains(id) && inode.nlink != 0 && !reported.contains(id)) lost.push_back(id);
      }
      if (lost.empty()) break;
      std::sort(lost.begin(), lost.end());
      std::vector<InodeId> tops;
      for (InodeId id : lost) {
        if (!named.contains(id)) tops.push_back(id);
      }
      // Only a cycle of directories naming each other is left: break it.
      if (tops.empty()) tops.push_back(lost.front());
      for (InodeId id : tops) {
        reported.insert(id);
        error(std::string(kind(*meta_->find(id))) + " " + std::to_string(id) +
              " is in no directory reachable from /");
        lost_and_found(id);
      }
      if (!options_.repair) {
        // Without repairs nothing moves: report the rest of the lost
        // subtrees' tops in one go.
        for (InodeId id : lost) reported.insert(id);
        break;
      }
    }
    if (orphans) {
      note(std::to_string(orphans) + " files deleted while still open " +
           (options_.repair ? "were freed" : "will be freed at the next mount"));
    }

    // Link counts, now that the tree is settled.
    std::unordered_map<InodeId, std::uint32_t> names, subdirs;
    for (const auto& [id, inode] : meta_->inodes) {
      for (const auto& [name, child] : inode.entries) {
        ++names[child];
        if (const Inode* c = meta_->find(child); c && c->is_dir()) ++subdirs[id];
      }
    }
    for (auto& [id, inode] : meta_->inodes) {
      if (inode.nlink == 0 && !reached.contains(id)) continue;  // an orphan
      std::uint32_t expected = inode.is_dir() ? 2 + subdirs[id] : names[id];
      if (inode.nlink != expected) {
        error(path_of(id) + ": link count is " + std::to_string(inode.nlink) + ", should be " +
              std::to_string(expected));
        InodeAttrs a = inode;
        a.nlink = expected;
        fix(op::PutInode{a});
      }
      if (inode.is_symlink() && inode.size != inode.symlink.size()) {
        error(path_of(id) + ": symlink size does not match its target");
        InodeAttrs a = inode;
        a.size = inode.symlink.size();
        fix(op::PutInode{a});
      }
      if (inode.is_regular()) ++report_.files;
      if (inode.is_dir()) ++report_.directories;
    }
  }

  std::unordered_set<InodeId> reachable_from_root() const {
    std::unordered_set<InodeId> seen = {kRootInode};
    std::deque<InodeId> queue = {kRootInode};
    while (!queue.empty()) {
      const Inode* d = meta_->find(queue.front());
      queue.pop_front();
      if (!d) continue;
      for (const auto& [name, child] : d->entries) {
        const Inode* c = meta_->find(child);
        if (!c || !seen.insert(child).second) continue;
        if (c->is_dir()) queue.push_back(child);
      }
    }
    return seen;
  }

  // Moves an unreachable inode (and so everything under it) into /lost+found.
  void lost_and_found(InodeId id) {
    if (!options_.repair) return;
    if (!lost_found_) {
      Inode* root = meta_->find(kRootInode);
      auto it = root->entries.find("lost+found");
      if (it != root->entries.end() && meta_->find(it->second)->is_dir()) {
        lost_found_ = it->second;
      } else {
        InodeAttrs a;
        a.id = meta_->next_inode;
        a.mode = S_IFDIR | 0700;
        a.nlink = 2;
        a.parent = kRootInode;
        a.atime_ns = a.mtime_ns = a.ctime_ns = a.btime_ns = static_cast<std::int64_t>(now_ns());
        fix(op::PutInode{a});
        fix(op::SetEntry{kRootInode, it == root->entries.end() ? "lost+found" : "lost+found.fsck", a.id});
        InodeAttrs r = *meta_->find(kRootInode);
        ++r.nlink;
        fix(op::PutInode{r});
        lost_found_ = a.id;
      }
    }
    // Detach it from any (unreachable) directory still naming it.
    for (auto& [dir_id, dir] : meta_->inodes) {
      std::vector<std::string> names;
      for (const auto& [name, child] : dir.entries) {
        if (child == id) names.push_back(name);
      }
      for (const std::string& n : names) fix(op::RemoveEntry{dir_id, n});
    }
    fix(op::SetEntry{*lost_found_, "#" + std::to_string(id), id});
    paths_[id] = "/lost+found/#" + std::to_string(id);
    if (meta_->find(id)->is_dir()) {
      InodeAttrs a = *meta_->find(id);
      a.parent = *lost_found_;
      fix(op::PutInode{a});
    }
    report_.fixed.push_back("reconnected inode " + std::to_string(id) + " as " + paths_[id]);
  }

  // Phase 4 (and 5 with --deep).
  void space() {
    const std::uint64_t cs = sb_.chunk_size();
    // Every record, with the references to it.
    struct Use {
      ChunkRef ref;  // the first reference seen
      std::uint32_t count = 0;
    };
    std::unordered_map<std::uint64_t, Use> uses;
    for (auto& [id, inode] : meta_->inodes) {
      for (const auto& [index, ref] : inode.chunks) {
        Use& u = uses.try_emplace(ref.offset, Use{ref, 0}).first->second;
        ++u.count;
      }
    }
    struct Bad {
      InodeId ino;
      std::uint64_t index;
    };
    std::vector<Bad> bad;
    std::unordered_set<std::uint64_t> seen;
    for (auto& [id, inode] : meta_->inodes) {
      for (const auto& [index, ref] : inode.chunks) {
        ++report_.chunks;
        if (index * cs >= inode.size) {
          error(path_of(id) + ": a chunk lies past the end of the file");
          bad.push_back({id, index});  // invisible data: nothing is lost
          continue;
        }
        const Use& u = uses.at(ref.offset);
        std::string problem;
        if (u.count > 1 && !meta_->shared_records()) {
          if (!seen.insert(ref.offset).second) problem = "shares its data with another file";
        } else if (u.ref.stored_size != ref.stored_size || u.ref.packed != ref.packed) {
          problem = "disagrees with another file about its data";
        }
        if (problem.empty()) problem = check_chunk(id, index, ref);
        if (!problem.empty()) {
          error(path_of(id) + ", " + range(index * cs, ref.size) + ": " + problem);
          report_.lost.push_back(path_of(id) + ": " + range(index * cs, ref.size));
          bad.push_back({id, index});
        }
      }
    }

    // Reference counts and space, from the references themselves: each
    // record's space counts once (without shared records, every reference
    // claims space of its own, even a wrong second one).
    std::unordered_map<std::uint64_t, std::uint32_t> counts;
    std::map<std::uint64_t, std::uint64_t> live;
    for (const auto& [offset, u] : uses) {
      if (u.ref.packed || u.count > 1) counts.emplace(offset, u.count);
      live[meta_->segment_of(offset)] += u.ref.stored_size * (meta_->shared_records() ? 1 : u.count);
    }
    bool counts_wrong = meta_->shared_records() && counts != meta_->shared;
    bool space_wrong = false;
    for (auto& [index, usage] : meta_->segments) {
      std::uint64_t expected = live.contains(index) ? live[index] : 0;
      if (usage.live_bytes != expected) space_wrong = true;
    }
    if (counts_wrong) error("the reference counts of shared data are wrong");
    if (space_wrong) error("the space accounting of segments is wrong");
    if (options_.repair && (counts_wrong || space_wrong)) {
      // Make them exact before removing anything, so removals count right.
      if (meta_->shared_records()) meta_->shared = counts;
      for (auto& [index, usage] : meta_->segments) usage.live_bytes = live.contains(index) ? live[index] : 0;
    }
    for (const Bad& b : bad) fix(op::RemoveChunk{b.ino, b.index});
  }

  // What the record at a location says, read once however many files
  // reference it.
  struct RecordCheck {
    std::string problem;
    DataChunkInfo info;
    std::uint64_t stored_size = 0;
    std::optional<Bytes> data;  // with --deep, for packs (pieces are hashed)
  };

  const RecordCheck& check_record(const ChunkRef& ref) {
    if (auto it = records_.find(ref.offset); it != records_.end()) return it->second;
    RecordCheck c;
    std::uint64_t seg = meta_->segment_of(ref.offset);
    auto usage = meta_->segments.find(seg);
    std::optional<Record> r;
    if (usage == meta_->segments.end()) {
      c.problem = "its data is in a free segment";
    } else {
      std::uint64_t limit = sb_.segment_offset(seg) + sb_.segment_size();
      r = options_.deep ? try_read_record(store_, ref.offset, limit, usage->second.seq)
                        : try_read_record_header(store_, ref.offset, limit, usage->second.seq);
      if (!r || r->header.type != RecordType::data_chunk) c.problem = "its data is missing or damaged";
    }
    if (c.problem.empty()) {
      try {
        c.info = parse_data_chunk(*r);
        c.stored_size = r->stored_size();
        if (options_.deep) {
          Bytes data = decode_data_chunk(*r);
          if (c.info.blake3 && blake3(data) != *c.info.blake3) throw Error(Errc::corrupt, "identity");
          if (c.info.pack) c.data = std::move(data);
          ++report_.chunks_verified;
        }
      } catch (const Error&) {
        c.problem = "its data is damaged";
      }
    }
    // Pack contents are kept only until all their pieces are checked; a
    // handful at a time is enough, as pieces of a pack are usually together.
    if (c.data) {
      packs_.push_back(ref.offset);
      if (packs_.size() > 64) {
        if (auto it = records_.find(packs_.front()); it != records_.end()) it->second.data.reset();
        packs_.pop_front();
      }
    }
    return records_.emplace(ref.offset, std::move(c)).first->second;
  }

  std::string check_chunk(InodeId id, std::uint64_t index, const ChunkRef& ref) {
    const RecordCheck& c = check_record(ref);
    if (!c.problem.empty()) return c.problem;
    const DataChunkInfo& info = c.info;
    if (info.pack != ref.packed) return "its data is not what it was written as";
    if (c.stored_size != ref.stored_size) return "its size does not match its data";
    if (ref.packed) {
      if (std::uint64_t{ref.piece} + ref.size > info.uncompressed_size) return "it lies outside its pack";
    } else if (info.uncompressed_size != ref.size) {
      return "its size does not match its data";
    }
    // Data that may be shared (it has an identity) or packed has no single
    // owner; any other data belongs to one file, at one position.
    if (!ref.packed && !ref.hash && !info.blake3 &&
        (info.inode != id || info.file_offset != index * sb_.chunk_size())) {
      return "its data belongs to another file";
    }
    if (ref.hash && !ref.packed && (!info.blake3 || hash_key(info.blake3->data()) != ref.hash)) {
      return "its identity does not match its data";
    }
    if (ref.hash && ref.packed && c.data) {
      Blake3Hash h = blake3(ByteView(*c.data).subspan(ref.piece, ref.size));
      if (hash_key(h.data()) != ref.hash) return "its identity does not match its data";
    }
    return {};
  }

  // Writes the repairs: one transaction, then a new checkpoint.
  void repair() {
    bool anything = !fixes_.empty() || new_checkpoint_ || rewrite_both_ || clear_errors_flag_;
    if (!anything) return;
    // Space accounting and reference counts are derived from the chunks;
    // make them exact (each record's space counts once).
    std::map<std::uint64_t, std::uint64_t> live;
    std::unordered_map<std::uint64_t, std::pair<ChunkRef, std::uint32_t>> uses;
    for (const auto& [id, inode] : meta_->inodes) {
      for (const auto& [index, ref] : inode.chunks) {
        auto& u = uses.try_emplace(ref.offset, ref, 0).first->second;
        if (u.second++ == 0 || !meta_->shared_records()) live[meta_->segment_of(ref.offset)] += ref.stored_size;
      }
    }
    for (auto& [index, usage] : meta_->segments) usage.live_bytes = live[index];
    if (meta_->shared_records()) {
      meta_->shared.clear();
      for (const auto& [offset, u] : uses) {
        if (u.first.packed || u.second > 1) meta_->shared.emplace(offset, u.second);
      }
    }

    std::uint64_t next_seq = std::max(sb_.next_segment_seq, replayed_->max_seq + 1);
    Journal journal(store_, sb_, *meta_, next_seq);
    journal.resume(replayed_->end);
    if (!fixes_.empty()) {
      std::uint64_t txid = meta_->last_txid + 1;
      journal.append_transaction(txid, fixes_);
      meta_->last_txid = txid;
    }
    journal.flush();
    LogPosition at = journal.append_checkpoint(*meta_);
    journal.flush();
    sb_.checkpoint_offset = at.offset;
    sb_.checkpoint_seq = at.seq;
    sb_.next_segment_seq = journal.next_seq();
    sb_.state &= ~state_flags::errors;
    write_superblock(store_, sb_);
    if (rewrite_both_) write_superblock(store_, sb_);  // a damaged slot gets a good copy too
    journal.checkpoint_done(at);

    // Everything found is now repaired (lost data stays listed in `lost`).
    for (std::string& e : report_.errors) report_.fixed.push_back(std::move(e));
    report_.errors.clear();
    if (clear_errors_flag_) report_.fixed.push_back("cleared the errors flag");
  }

  Store& store_;
  const FsckOptions& options_;
  FsckReport& report_;
  std::optional<LoadedSuperblock> loaded_;
  Superblock sb_;
  std::optional<ReplayResult> replayed_;
  std::optional<Metadata> meta_;
  std::vector<Op> fixes_;
  std::unordered_map<InodeId, std::string> paths_;
  std::optional<InodeId> lost_found_;
  std::unordered_map<std::uint64_t, RecordCheck> records_;
  std::deque<std::uint64_t> packs_;
  bool new_checkpoint_ = false;
  bool rewrite_both_ = false;
  bool clear_errors_flag_ = false;
};

}  // namespace

FsckReport fsck(Store& store, const FsckOptions& options) {
  FsckReport report;
  Checker(store, options, report).run();
  return report;
}

}  // namespace nanoer
