// Compression codecs. See KNOWLEDGE.md §3.
//
// Every chunk is compressed on its own. Codec ids are part of the on-disk
// format and must never be renumbered.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "nanoer/bytes.h"

namespace nanoer {

enum class CodecId : std::uint8_t {
  none = 0,  // stored raw
  lz4 = 1,
  zstd = 2,
  lzma = 3,  // .xz container, no integrity check (Nanoer checksums itself)
};

std::string_view codec_name(CodecId codec);
std::optional<CodecId> codec_from_name(std::string_view name);
std::optional<CodecId> codec_from_id(std::uint8_t id);

struct LevelRange {
  int min;
  int max;
  int fallback;  // the level used when the caller has no preference
};
LevelRange codec_levels(CodecId codec);

// Upper bound on compress() output for `size` input bytes.
std::size_t compress_bound(CodecId codec, std::size_t size);

// Compresses `src` into `dst` (which must hold compress_bound() bytes) and
// returns the compressed size. Throws Error(invalid_argument) for a level
// outside codec_levels().
std::size_t compress(CodecId codec, int level, ByteView src, MutableByteView dst);
Bytes compress(CodecId codec, int level, ByteView src);

// Decompresses `src` into exactly dst.size() bytes. Throws Error(corrupt) if
// the input is malformed or does not decode to exactly that many bytes.
void decompress(CodecId codec, ByteView src, MutableByteView dst);
Bytes decompress(CodecId codec, ByteView src, std::size_t uncompressed_size);

// A chunk ready to be written: compressed, or stored raw when compression
// would not make it smaller.
struct EncodedChunk {
  CodecId codec;
  std::uint32_t uncompressed_size;
  std::uint64_t xxh3;  // of the uncompressed data
  Bytes payload;
};

EncodedChunk encode_chunk(ByteView data, CodecId codec, int level);

// Decompresses and verifies a chunk. Throws Error(corrupt) on any mismatch.
Bytes decode_chunk(CodecId codec, ByteView payload, std::uint32_t uncompressed_size,
                   std::uint64_t xxh3);

}  // namespace nanoer
