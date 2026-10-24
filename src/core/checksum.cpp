#include "nanoer/checksum.h"

#include <array>
#include <cstring>

#include <blake3.h>
#include <xxhash.h>

#if defined(__x86_64__)
#include <nmmintrin.h>
#elif defined(__aarch64__)
#include <arm_acle.h>
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#endif

namespace nanoer {
namespace {

// Reflected Castagnoli polynomial.
constexpr std::uint32_t kPoly = 0x82f63b78;

// Slicing-by-8 tables: table[k][b] is the CRC of byte b followed by k zero
// bytes, which lets the software path consume 8 bytes per step.
using Tables = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Tables make_tables() {
  Tables t{};
  for (std::uint32_t b = 0; b < 256; ++b) {
    std::uint32_t crc = b;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (kPoly & (0u - (crc & 1)));
    t[0][b] = crc;
  }
  for (std::size_t k = 1; k < 8; ++k) {
    for (std::size_t b = 0; b < 256; ++b) {
      t[k][b] = (t[k - 1][b] >> 8) ^ t[0][t[k - 1][b] & 0xff];
    }
  }
  return t;
}

constexpr Tables kTables = make_tables();

std::uint32_t crc_software_raw(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
  while (n >= 8) {
    std::uint32_t lo = crc ^ load_le<std::uint32_t>(p);
    std::uint32_t hi = load_le<std::uint32_t>(p + 4);
    crc = kTables[7][lo & 0xff] ^ kTables[6][(lo >> 8) & 0xff] ^
          kTables[5][(lo >> 16) & 0xff] ^ kTables[4][lo >> 24] ^
          kTables[3][hi & 0xff] ^ kTables[2][(hi >> 8) & 0xff] ^
          kTables[1][(hi >> 16) & 0xff] ^ kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) crc = (crc >> 8) ^ kTables[0][(crc ^ *p++) & 0xff];
  return crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
std::uint32_t crc_hardware_raw(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
  std::uint64_t c = crc;
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);  // x86 is little-endian, as the CRC expects
    c = _mm_crc32_u64(c, v);
    p += 8;
    n -= 8;
  }
  auto c32 = static_cast<std::uint32_t>(c);
  while (n--) c32 = _mm_crc32_u8(c32, *p++);
  return c32;
}

bool detect_hardware() { return __builtin_cpu_supports("sse4.2"); }
#elif defined(__aarch64__)
__attribute__((target("+crc")))
std::uint32_t crc_hardware_raw(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    crc = __crc32cd(crc, v);
    p += 8;
    n -= 8;
  }
  while (n--) crc = __crc32cb(crc, *p++);
  return crc;
}

bool detect_hardware() { return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0; }
#else
std::uint32_t crc_hardware_raw(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
  return crc_software_raw(p, n, crc);
}

bool detect_hardware() { return false; }
#endif

const bool kHardware = detect_hardware();

}  // namespace

namespace detail {
std::uint32_t crc32c_software(ByteView data, std::uint32_t crc) {
  return ~crc_software_raw(data.data(), data.size(), ~crc);
}
}  // namespace detail

std::uint32_t crc32c(ByteView data, std::uint32_t crc) {
  if (!kHardware) return detail::crc32c_software(data, crc);
  return ~crc_hardware_raw(data.data(), data.size(), ~crc);
}

bool crc32c_hardware_accelerated() { return kHardware; }

std::uint64_t xxh3_64(ByteView data) { return XXH3_64bits(data.data(), data.size()); }

Blake3Hash blake3(ByteView data) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data.data(), data.size());
  Blake3Hash out;
  blake3_hasher_finalize(&hasher, out.data(), out.size());
  return out;
}

}  // namespace nanoer
