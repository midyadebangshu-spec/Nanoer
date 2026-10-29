#include "nanoer/upgrade.h"

#include <algorithm>
#include <memory>
#include <string>

#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/journal.h"
#include "nanoer/legacy_metadata.h"
#include "nanoer/metadata.h"
#include "nanoer/record.h"

namespace nanoer {

namespace {

// Does the header of the record `ref` names its one user (ino, index)? GC
// finds a record's single user from its header; a record dedup shared once
// may have outlived the user its header names.
bool header_names(Store& store, const Superblock& sb, const LegacyMetadata& meta, const ChunkRef& ref, InodeId ino,
                  std::uint64_t index) {
  std::uint64_t seg = meta.segment_of(ref.offset);
  auto usage = meta.segments.find(seg);
  if (usage == meta.segments.end()) return false;
  std::optional<Record> r =
      try_read_record_header(store, ref.offset, sb.segment_offset(seg) + sb.segment_size(), usage->second.seq);
  if (!r || r->header.type != RecordType::data_chunk) return false;
  try {
    DataChunkInfo info = parse_data_chunk(*r);
    return !info.pack && info.inode == ino && info.file_offset == index * sb.chunk_size();
  } catch (const Error&) {
    return false;
  }
}

}  // namespace

UpgradeReport upgrade(Store& store) {
  UpgradeReport report;
  LoadedSuperblock loaded = load_superblock(store);
  const Superblock old_sb = loaded.sb;
  if (has_metadata_tree(old_sb)) {
    report.already = true;
    return report;
  }
  if (loaded.read_only) throw Error(Errc::unsupported, "the filesystem has features this version cannot write");
  if (old_sb.state & state_flags::errors) {
    throw Error(Errc::corrupt, "the filesystem recorded damage; run nanoer fsck --repair first");
  }
  ReplayResult r = [&] {
    try {
      return replay(store, old_sb);
    } catch (const Error& e) {
      throw Error(Errc::corrupt, std::string(e.what()) + "; run nanoer fsck first");
    }
  }();
  if (r.damage.found) throw Error(Errc::corrupt, "the log is damaged; run nanoer fsck --repair first");
  if (!r.legacy) throw Error(Errc::invalid_argument, "not a filesystem from before the metadata tree");
  const LegacyMetadata& old = *r.legacy;
  report.old_checkpoint = r.checkpoint_bytes;

  // The new features: the tree, and shared records and cold data for
  // filesystems from before them (an M2 or M3 filesystem simply has none).
  Superblock sb = old_sb;
  sb.features_incompat |=
      incompat_features::shared_records | incompat_features::cold_data | incompat_features::metadata_tree;

  // The same metadata, with shared records allowed, and a list of users for
  // every record whose header does not name its one user.
  LegacyMetadata meta(old.data_start(), old.segment_shift(), true, true);
  meta.inodes = old.inodes;
  meta.segments = old.segments;
  meta.shared = old.shared;
  meta.dedup = old.dedup;
  meta.next_inode = old.next_inode;
  meta.last_txid = old.last_txid;
  for (const auto& [id, inode] : meta.inodes) {
    ++report.inodes;
    for (const auto& [index, ref] : inode.chunks) {
      ++report.chunks;
      if (ref.packed || meta.shared.contains(ref.offset)) continue;
      if (!header_names(store, old_sb, meta, ref, id, index)) {
        meta.shared[ref.offset] = 1;
        ++report.named_users;
      }
    }
  }

  // Room for the tree: about as much as the old checkpoint, twice over to
  // be safe, and the segments every mount keeps in reserve.
  const std::uint64_t usable = sb.segment_size() - 2 * kSegmentHeaderSize;
  const std::uint64_t needed = (2 * r.checkpoint_bytes + usable - 1) / usable + 4;
  const std::uint64_t free = sb.segment_count - std::min<std::uint64_t>(sb.segment_count, meta.segments.size());
  if (free < needed) {
    throw Error(Errc::no_space, "the upgrade needs " + std::to_string(needed) + " free segments of " +
                                    std::to_string(sb.segment_size() >> 20) + " MiB, and " +
                                    std::to_string(free) + " are free; delete some files first");
  }

  // The tree, written after the old log (whose end is sealed, as at every
  // mount), then its checkpoint. Until the superblock below, nothing the
  // old filesystem uses is touched.
  auto io = std::make_shared<LogNodeIO>(store, sb);
  Metadata tree = Metadata::from_legacy(meta, metadata_options(sb), io);
  Journal journal(store, sb, tree.segments, std::max(old_sb.next_segment_seq, r.max_seq + 1));
  io->set_journal(&journal);
  journal.resume(r.end, tree.last_txid);
  LogPosition at = journal.append_checkpoint(tree);
  journal.flush();
  report.node_bytes = tree.counters.node_bytes;

  // Read back from the store before switching over.
  Metadata back(sb.data_start, sb.segment_shift, metadata_options(sb), std::make_shared<LogNodeIO>(store, sb),
                tree.tree().root());
  back.segments = tree.segments;
  back.counters = tree.counters;
  back.next_inode = tree.next_inode;
  back.last_txid = tree.last_txid;
  if (!back.same_as(meta)) {
    throw Error(Errc::corrupt, "the upgraded metadata does not read back the same; the filesystem is unchanged");
  }

  sb.checkpoint_offset = at.offset;
  sb.checkpoint_seq = at.seq;
  sb.next_segment_seq = journal.next_seq();
  write_superblock(store, sb);
  return report;
}

}  // namespace nanoer
