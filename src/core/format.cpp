#include "nanoer/format.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <sys/random.h>

#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

constexpr char kMagic[8] = {'N', 'A', 'N', 'O', 'E', 'R', 'F', 'S'};

// Field offsets inside the 4 KiB superblock; see docs/format.md.
namespace off {
constexpr std::size_t magic = 0;
constexpr std::size_t version_major = 8;
constexpr std::size_t version_minor = 10;
constexpr std::size_t block_size = 12;
constexpr std::size_t uuid = 16;
constexpr std::size_t generation = 32;
constexpr std::size_t features_compat = 40;
constexpr std::size_t features_ro_compat = 48;
constexpr std::size_t features_incompat = 56;
constexpr std::size_t store_size = 64;
constexpr std::size_t data_start = 72;
constexpr std::size_t segment_shift = 80;
constexpr std::size_t segment_count = 88;
constexpr std::size_t created_ns = 96;
constexpr std::size_t updated_ns = 104;
constexpr std::size_t next_segment_seq = 112;
constexpr std::size_t checkpoint_offset = 120;
constexpr std::size_t checkpoint_seq = 128;
constexpr std::size_t default_codec = 136;
constexpr std::size_t default_level = 137;
constexpr std::size_t chunk_shift = 138;
constexpr std::size_t state = 140;
constexpr std::size_t label = 144;
constexpr std::size_t label_size = 64;
constexpr std::size_t crc = kBlockSize - 4;
}  // namespace off

DecodedSuperblock invalid(std::string reason) {
  DecodedSuperblock d;
  d.state = DecodedSuperblock::State::invalid;
  d.reason = std::move(reason);
  return d;
}

// Field checks for a superblock whose checksum is already known to be good.
// Returns an empty string when everything is consistent.
std::string check_fields(const Superblock& sb) {
  if (sb.block_size != kBlockSize) return "unsupported block size";
  if (sb.segment_shift < kMinSegmentShift || sb.segment_shift > kMaxSegmentShift) {
    return "segment size out of range";
  }
  if (sb.chunk_shift < kMinChunkShift || sb.chunk_shift > kMaxChunkShift) {
    return "chunk size out of range";
  }
  if (sb.chunk_shift >= sb.segment_shift) return "chunk size not smaller than segment size";
  auto codec = codec_from_id(static_cast<std::uint8_t>(sb.default_codec));
  if (!codec) return "unknown default codec";
  LevelRange levels = codec_levels(*codec);
  if (sb.default_level < levels.min || sb.default_level > levels.max) {
    return "default level out of range";
  }
  if (sb.data_start < 2 * kBlockSize || sb.data_start % kBlockSize != 0) {
    return "bad data start";
  }
  if (sb.data_start > sb.store_size) return "data start beyond the end of the store";
  if (sb.segment_count < kMinSegmentCount) return "too few segments";
  if (sb.segment_count > (sb.store_size - sb.data_start) >> sb.segment_shift) {
    return "segments extend beyond the end of the store";
  }
  if (sb.next_segment_seq == 0) return "bad next segment sequence";
  std::uint64_t data_end = sb.segment_offset(sb.segment_count);
  if (sb.checkpoint_offset != 0 &&
      (sb.checkpoint_offset < sb.data_start || sb.checkpoint_offset >= data_end)) {
    return "checkpoint location out of range";
  }
  return {};
}

}  // namespace

Uuid random_uuid() {
  Uuid u;
  std::size_t done = 0;
  while (done < u.size()) {
    ssize_t n = ::getrandom(u.data() + done, u.size() - done, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw Error(Errc::io, "getrandom failed", errno);
    }
    done += static_cast<std::size_t>(n);
  }
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0f) | 0x40);  // RFC 9562 version 4
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3f) | 0x80);  // variant 10
  return u;
}

std::string uuid_to_string(const Uuid& u) {
  std::string hex = to_hex(u);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
         hex.substr(16, 4) + "-" + hex.substr(20);
}

std::uint64_t now_ns() {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return static_cast<std::uint64_t>(ns.count());
}

Bytes encode_superblock(const Superblock& sb) {
  if (sb.label.size() > kMaxLabelLength) {
    throw Error(Errc::invalid_argument, "label longer than 63 bytes");
  }
  Bytes b(kBlockSize, 0);
  std::memcpy(b.data() + off::magic, kMagic, sizeof kMagic);
  store_le(b.data() + off::version_major, sb.version_major);
  store_le(b.data() + off::version_minor, sb.version_minor);
  store_le(b.data() + off::block_size, sb.block_size);
  std::memcpy(b.data() + off::uuid, sb.uuid.data(), sb.uuid.size());
  store_le(b.data() + off::generation, sb.generation);
  store_le(b.data() + off::features_compat, sb.features_compat);
  store_le(b.data() + off::features_ro_compat, sb.features_ro_compat);
  store_le(b.data() + off::features_incompat, sb.features_incompat);
  store_le(b.data() + off::store_size, sb.store_size);
  store_le(b.data() + off::data_start, sb.data_start);
  store_le(b.data() + off::segment_shift, sb.segment_shift);
  store_le(b.data() + off::segment_count, sb.segment_count);
  store_le(b.data() + off::created_ns, sb.created_ns);
  store_le(b.data() + off::updated_ns, sb.updated_ns);
  store_le(b.data() + off::next_segment_seq, sb.next_segment_seq);
  store_le(b.data() + off::checkpoint_offset, sb.checkpoint_offset);
  store_le(b.data() + off::checkpoint_seq, sb.checkpoint_seq);
  b[off::default_codec] = static_cast<std::uint8_t>(sb.default_codec);
  b[off::default_level] = static_cast<std::uint8_t>(sb.default_level);
  b[off::chunk_shift] = sb.chunk_shift;
  store_le(b.data() + off::state, sb.state);
  std::memcpy(b.data() + off::label, sb.label.data(), sb.label.size());
  store_le(b.data() + off::crc, crc32c(ByteView(b.data(), off::crc)));
  return b;
}

DecodedSuperblock decode_superblock(ByteView b) {
  if (b.size() != kBlockSize) return invalid("wrong size");
  if (std::memcmp(b.data() + off::magic, kMagic, sizeof kMagic) != 0) {
    return invalid("not a Nanoer superblock");
  }
  if (load_le<std::uint32_t>(b.data() + off::crc) != crc32c(b.first(off::crc))) {
    return invalid("checksum mismatch");
  }

  DecodedSuperblock d;
  Superblock& sb = d.sb;
  sb.version_major = load_le<std::uint16_t>(b.data() + off::version_major);
  sb.version_minor = load_le<std::uint16_t>(b.data() + off::version_minor);
  sb.block_size = load_le<std::uint32_t>(b.data() + off::block_size);
  std::memcpy(sb.uuid.data(), b.data() + off::uuid, sb.uuid.size());
  sb.generation = load_le<std::uint64_t>(b.data() + off::generation);
  sb.features_compat = load_le<std::uint64_t>(b.data() + off::features_compat);
  sb.features_ro_compat = load_le<std::uint64_t>(b.data() + off::features_ro_compat);
  sb.features_incompat = load_le<std::uint64_t>(b.data() + off::features_incompat);
  sb.store_size = load_le<std::uint64_t>(b.data() + off::store_size);
  sb.data_start = load_le<std::uint64_t>(b.data() + off::data_start);
  sb.segment_shift = load_le<std::uint32_t>(b.data() + off::segment_shift);
  sb.segment_count = load_le<std::uint64_t>(b.data() + off::segment_count);
  sb.created_ns = load_le<std::uint64_t>(b.data() + off::created_ns);
  sb.updated_ns = load_le<std::uint64_t>(b.data() + off::updated_ns);
  sb.next_segment_seq = load_le<std::uint64_t>(b.data() + off::next_segment_seq);
  sb.checkpoint_offset = load_le<std::uint64_t>(b.data() + off::checkpoint_offset);
  sb.checkpoint_seq = load_le<std::uint64_t>(b.data() + off::checkpoint_seq);
  sb.default_codec = static_cast<CodecId>(b[off::default_codec]);
  sb.default_level = static_cast<std::int8_t>(b[off::default_level]);
  sb.chunk_shift = b[off::chunk_shift];
  sb.state = load_le<std::uint32_t>(b.data() + off::state);

  const auto* label = reinterpret_cast<const char*>(b.data() + off::label);
  std::size_t label_len = strnlen(label, off::label_size);
  if (label_len > kMaxLabelLength) return invalid("label not terminated");
  sb.label.assign(label, label_len);

  // From here on the block is intact. A newer format is "unsupported", not
  // "invalid": the caller must not fall back to an older superblock.
  if (sb.version_major != kVersionMajor) {
    d.state = DecodedSuperblock::State::unsupported;
    d.reason = "format version " + std::to_string(sb.version_major) + "." +
               std::to_string(sb.version_minor) + " needs a newer Nanoer";
    return d;
  }
  if (std::uint64_t unknown = sb.features_incompat & ~kKnownIncompatFeatures) {
    d.state = DecodedSuperblock::State::unsupported;
    char buf[64];
    std::snprintf(buf, sizeof buf, "unknown incompatible features 0x%llx",
                  static_cast<unsigned long long>(unknown));
    d.reason = buf;
    return d;
  }
  if (std::string problem = check_fields(sb); !problem.empty()) return invalid(problem);
  d.state = DecodedSuperblock::State::valid;
  return d;
}

LoadedSuperblock load_superblock(Store& store) {
  if (store.size() < kSuperblockOffsets[1] + kBlockSize) {
    throw Error(Errc::corrupt, "store too small to hold a Nanoer filesystem");
  }
  DecodedSuperblock slots[2];
  for (int i = 0; i < 2; ++i) {
    slots[i] = decode_superblock(store.read(kSuperblockOffsets[i], kBlockSize));
  }

  int newest = -1;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].state == DecodedSuperblock::State::invalid) continue;
    if (newest < 0 || slots[i].sb.generation > slots[newest].sb.generation) newest = i;
  }
  if (newest < 0) {
    throw Error(Errc::corrupt, "no valid superblock (slot 0: " + slots[0].reason +
                                   "; slot 1: " + slots[1].reason + ")");
  }
  const DecodedSuperblock& chosen = slots[newest];
  if (chosen.state == DecodedSuperblock::State::unsupported) {
    throw Error(Errc::unsupported, chosen.reason);
  }
  if (chosen.sb.store_size > store.size()) {
    throw Error(Errc::corrupt, "the store is smaller than the filesystem on it (truncated?)");
  }
  bool read_only = (chosen.sb.features_ro_compat & ~kKnownRoCompatFeatures) != 0;
  LoadedSuperblock loaded{chosen.sb, newest, read_only, std::nullopt};
  const DecodedSuperblock& other = slots[1 - newest];
  if (other.state == DecodedSuperblock::State::valid && other.sb.generation < chosen.sb.generation &&
      other.sb.uuid == chosen.sb.uuid && other.sb.store_size <= store.size()) {
    loaded.older = other.sb;
  }
  return loaded;
}

void write_superblock(Store& store, Superblock& sb) {
  ++sb.generation;
  sb.updated_ns = now_ns();
  Bytes block = encode_superblock(sb);
  store.write(kSuperblockOffsets[sb.generation % 2], block);
  store.flush();
}

}  // namespace nanoer
