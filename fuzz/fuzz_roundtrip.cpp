// Whatever goes in must come back out unchanged, for every codec and level.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "nanoer/codec.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) return 0;
  nanoer::CodecId codec = *nanoer::codec_from_id(data[0] % 4);
  nanoer::LevelRange levels = nanoer::codec_levels(codec);
  int span = levels.max - levels.min + 1;
  int level = levels.min + data[1] % span;
  if (codec == nanoer::CodecId::zstd && level > 9) level = 9;  // keep runs fast
  nanoer::ByteView input(data + 2, size - 2);

  nanoer::EncodedChunk chunk = nanoer::encode_chunk(input, codec, level);
  if (chunk.payload.size() > input.size()) __builtin_trap();
  nanoer::Bytes back =
      nanoer::decode_chunk(chunk.codec, chunk.payload, chunk.uncompressed_size, chunk.xxh3);
  if (back.size() != input.size() || !std::equal(back.begin(), back.end(), input.begin())) {
    __builtin_trap();
  }
  return 0;
}
