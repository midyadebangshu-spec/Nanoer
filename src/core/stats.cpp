#include "nanoer/stats.h"

#include <set>
#include <sstream>
#include <unordered_map>

#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/record.h"
#include "nanoer/segment.h"

namespace nanoer {
namespace {

// The fields, in order, for encoding and decoding.
template <class F>
void each_field(SpaceStats& s, F&& f) {
  f("files", s.files);
  f("directories", s.directories);
  f("other_inodes", s.other_inodes);
  f("logical_bytes", s.logical_bytes);
  f("referenced_bytes", s.referenced_bytes);
  f("dedup_bytes", s.dedup_bytes);
  f("unique_bytes", s.unique_bytes);
  f("pack_dead_bytes", s.pack_dead_bytes);
  f("stored_bytes", s.stored_bytes);
  f("records", s.records);
  f("shared_records", s.shared_records);
  f("packs", s.packs);
  f("packed_files", s.packed_files);
  f("segment_bytes", s.segment_bytes);
  f("segments", s.segments);
  f("segments_in_use", s.segments_in_use);
  f("segments_pending", s.segments_pending);
  f("segments_free", s.segments_free);
  f("live_bytes", s.live_bytes);
  f("gc_bytes_copied", s.gc_bytes_copied);
  f("gc_records_copied", s.gc_records_copied);
  f("gc_segments_emptied", s.gc_segments_emptied);
  f("gc_packs_repacked", s.gc_packs_repacked);
  f("gc_damaged", s.gc_damaged);
  f("dedup_entries", s.dedup_entries);
  f("dedup_index_bytes", s.dedup_index_bytes);
  f("metadata_bytes", s.metadata_bytes);
}

}  // namespace

std::string encode_stats(const SpaceStats& stats) {
  SpaceStats s = stats;
  std::ostringstream out;
  each_field(s, [&](const char* name, std::uint64_t& v) { out << name << '=' << v << '\n'; });
  out << "dedup=" << (s.dedup ? 1 : 0) << '\n';
  for (const auto& [codec, c] : s.by_codec) {
    out << "codec." << codec << '=' << c.records << ',' << c.data_bytes << ',' << c.stored_bytes << '\n';
  }
  return out.str();
}

SpaceStats decode_stats(const std::string& text) {
  SpaceStats s;
  std::map<std::string, std::string> kv;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    auto eq = line.find('=');
    if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  auto number = [](const std::string& v) {
    try {
      return static_cast<std::uint64_t>(std::stoull(v));
    } catch (const std::exception&) {
      throw Error(Errc::corrupt, "malformed statistics");
    }
  };
  each_field(s, [&](const char* name, std::uint64_t& v) {
    if (auto it = kv.find(name); it != kv.end()) v = number(it->second);
  });
  s.dedup = kv["dedup"] == "1";
  for (const auto& [key, value] : kv) {
    if (!key.starts_with("codec.")) continue;
    SpaceStats::Codec c;
    std::istringstream v(value);
    std::string a, b, d;
    std::getline(v, a, ',');
    std::getline(v, b, ',');
    std::getline(v, d, ',');
    c.records = number(a);
    c.data_bytes = number(b);
    c.stored_bytes = number(d);
    s.by_codec[key.substr(6)] = c;
  }
  return s;
}

SpaceStats Filesystem::space_stats(bool by_codec) {
  std::shared_lock lock(mutex_);
  return space_stats_locked(by_codec);
}

SpaceStats Filesystem::space_stats_locked(bool by_codec) {
  SpaceStats s;
  struct Use {
    std::uint32_t stored = 0;
    bool packed = false;
    std::uint32_t size = 0;  // not packed
    std::uint64_t refs = 0;
    std::uint64_t ref_bytes = 0;
    std::set<std::pair<std::uint32_t, std::uint32_t>> pieces;
  };
  std::unordered_map<std::uint64_t, Use> uses;
  std::uint64_t chunk_refs = 0, entries = 0, xattr_bytes = 0;
  for (const auto& [id, inode] : meta_.inodes) {
    if (inode.is_regular()) {
      ++s.files;
      s.logical_bytes += inode.size;
    } else if (inode.is_dir()) {
      ++s.directories;
    } else {
      ++s.other_inodes;
    }
    for (const auto& [name, child] : inode.entries) entries += name.size();
    for (const auto& [name, value] : inode.xattrs) xattr_bytes += name.size() + value.size();
    for (const auto& [index, ref] : inode.chunks) {
      ++chunk_refs;
      s.referenced_bytes += ref.size;
      Use& u = uses[ref.offset];
      u.stored = ref.stored_size;
      u.packed = ref.packed;
      ++u.refs;
      u.ref_bytes += ref.size;
      if (ref.packed) {
        u.pieces.emplace(ref.piece, ref.size);
        ++s.packed_files;
      } else {
        u.size = ref.size;
      }
    }
  }
  for (const auto& [offset, u] : uses) {
    ++s.records;
    s.stored_bytes += u.stored;
    std::uint64_t distinct = u.size;
    std::uint64_t data = u.size;
    std::optional<DataChunkInfo> info;
    // Packs hold their whole data, dead pieces too: the header says how much.
    if (u.packed || by_codec) {
      std::uint64_t segment = meta_.segment_of(offset);
      if (auto it = meta_.segments.find(segment); it != meta_.segments.end()) {
        try {
          auto r = try_read_record_header(store_, offset, sb_.segment_offset(segment) + sb_.segment_size(),
                                          it->second.seq);
          if (r && r->header.type == RecordType::data_chunk) info = parse_data_chunk(*r);
        } catch (const Error&) {
        }
      }
    }
    if (u.packed) {
      ++s.packs;
      distinct = 0;
      for (const auto& [piece, size] : u.pieces) distinct += size;
      data = info ? std::max<std::uint64_t>(info->uncompressed_size, distinct) : distinct;
      s.pack_dead_bytes += data - distinct;
    }
    s.unique_bytes += data;
    s.dedup_bytes += u.ref_bytes - distinct;
    if (u.ref_bytes > distinct) ++s.shared_records;  // (a pack's several pieces are not sharing)
    if (by_codec && info) {
      SpaceStats::Codec& c = s.by_codec[std::string(codec_name(info->codec))];
      ++c.records;
      c.data_bytes += data;
      c.stored_bytes += u.stored;
    }
  }

  s.segment_bytes = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  s.segments = sb_.segment_count;
  s.segments_in_use = meta_.segments.size();
  s.segments_pending = journal_ ? journal_->pending_free_segments() : 0;
  s.segments_free = journal_ ? journal_->free_segments() : sb_.segment_count - meta_.segments.size();
  for (const auto& [index, usage] : meta_.segments) s.live_bytes += usage.live_bytes;

  s.gc_bytes_copied = gc_stats_.bytes_copied;
  s.gc_records_copied = gc_stats_.records_copied;
  s.gc_segments_emptied = gc_stats_.segments_emptied;
  s.gc_packs_repacked = gc_stats_.packs_repacked;
  s.gc_damaged = gc_stats_.damaged;

  s.dedup = dedup_ && !read_only_;
  s.dedup_entries = meta_.dedup.size();
  s.dedup_index_bytes = meta_.dedup.memory_bytes();
  // Rough sizes of the in-memory structures (node-based maps): an inode
  // ~200 bytes, a chunk reference ~80, a directory entry ~64 plus its name.
  s.metadata_bytes = meta_.inodes.size() * 200 + chunk_refs * 80 + entries + xattr_bytes +
                     meta_.shared.size() * 32 + meta_.segments.size() * 64 + s.dedup_index_bytes;
  for (const auto& [id, inode] : meta_.inodes) s.metadata_bytes += inode.entries.size() * 64 + inode.xattrs.size() * 64;
  return s;
}

}  // namespace nanoer
