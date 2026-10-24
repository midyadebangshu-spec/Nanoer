#include "nanoer/mkfs.h"

#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {

std::uint64_t min_store_size(unsigned segment_shift) {
  return kDataStart + kMinSegmentCount * (std::uint64_t{1} << segment_shift);
}

Superblock make_filesystem(Store& store, const FormatOptions& options) {
  if (options.segment_shift < kMinSegmentShift || options.segment_shift > kMaxSegmentShift) {
    throw Error(Errc::invalid_argument, "segment size must be a power of two from 1 MiB to 1 GiB");
  }
  if (options.chunk_shift < kMinChunkShift || options.chunk_shift > kMaxChunkShift) {
    throw Error(Errc::invalid_argument, "chunk size must be a power of two from 4 KiB to 16 MiB");
  }
  if (options.chunk_shift >= options.segment_shift) {
    throw Error(Errc::invalid_argument, "chunk size must be smaller than the segment size");
  }
  LevelRange levels = codec_levels(options.codec);
  if (options.level < levels.min || options.level > levels.max) {
    throw Error(Errc::invalid_argument, "compression level out of range for " +
                                            std::string(codec_name(options.codec)));
  }
  if (options.label.size() > kMaxLabelLength) {
    throw Error(Errc::invalid_argument, "label longer than 63 bytes");
  }
  if (store.size() < min_store_size(options.segment_shift)) {
    throw Error(Errc::invalid_argument,
                "store too small: need at least " +
                    std::to_string(min_store_size(options.segment_shift)) + " bytes");
  }

  Superblock sb;
  sb.uuid = options.uuid ? *options.uuid : random_uuid();
  sb.store_size = store.size();
  sb.segment_shift = options.segment_shift;
  sb.segment_count = (store.size() - kDataStart) >> options.segment_shift;
  sb.created_ns = now_ns();
  sb.default_codec = options.codec;
  sb.default_level = static_cast<std::int8_t>(options.level);
  sb.chunk_shift = static_cast<std::uint8_t>(options.chunk_shift);
  sb.label = options.label;

  // Clear the reserved area so nothing from an earlier filesystem survives
  // there. Segments need no clearing: their headers name another
  // filesystem's UUID (or none), so they read as free. This keeps mkfs
  // instant even on a large disk.
  Bytes zeros(kDataStart, 0);
  store.write(0, zeros);

  // Fill both superblock slots (generations 1 and 2), so the store never
  // has an empty slot that could be mistaken for damage.
  write_superblock(store, sb);
  write_superblock(store, sb);
  return sb;
}

}  // namespace nanoer
