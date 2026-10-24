// Arbitrary bytes fed to every decompressor must be rejected cleanly
// (Error(corrupt)), never crash, hang or read out of bounds.
#include <cstddef>
#include <cstdint>

#include "nanoer/bytes.h"
#include "nanoer/codec.h"
#include "nanoer/error.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 4) return 0;
  auto codec = nanoer::codec_from_id(data[0] % 4);
  // The claimed decompressed size.
  std::size_t out_size = nanoer::load_le<std::uint32_t>(data) >> 8;
  out_size %= (1u << 20) + 1;  // large enough for every code path, small enough to run fast
  nanoer::ByteView input(data + 4, size - 4);
  try {
    nanoer::Bytes out = nanoer::decompress(*codec, input, out_size);
    if (out.size() != out_size) __builtin_trap();
  } catch (const nanoer::Error& e) {
    if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
  }
  return 0;
}
