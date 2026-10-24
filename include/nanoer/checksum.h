// Checksums and hashes. See KNOWLEDGE.md §5 for why each one is used where.
//
//   CRC-32C  structure headers and record payloads (hardware-accelerated)
//   XXH3-64  uncompressed chunk contents, verified after decompression
//   BLAKE3   content identity for deduplication (collision-resistant)
#pragma once

#include <array>
#include <cstdint>

#include "nanoer/bytes.h"

namespace nanoer {

// CRC-32C (Castagnoli). Pass a previous result as `crc` to continue a
// running checksum: crc32c(b, crc32c(a)) == crc32c(a + b).
std::uint32_t crc32c(ByteView data, std::uint32_t crc = 0);

// True when crc32c() uses CPU instructions (SSE4.2 or ARMv8 CRC).
bool crc32c_hardware_accelerated();

namespace detail {
std::uint32_t crc32c_software(ByteView data, std::uint32_t crc);
}

std::uint64_t xxh3_64(ByteView data);

using Blake3Hash = std::array<std::uint8_t, 32>;
Blake3Hash blake3(ByteView data);

}  // namespace nanoer
