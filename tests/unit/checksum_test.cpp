#include "nanoer/checksum.h"

#include <algorithm>
#include <random>
#include <string>

#include <gtest/gtest.h>

using namespace nanoer;

namespace {

Bytes random_bytes(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

}  // namespace

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(crc32c(ByteView()), 0u);
  EXPECT_EQ(crc32c(as_bytes("123456789")), 0xe3069283u);  // the standard check value
  // RFC 3720 (iSCSI) appendix B.4.
  Bytes zeros(32, 0x00), ones(32, 0xff), up(32), down(32);
  for (int i = 0; i < 32; ++i) {
    up[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    down[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(31 - i);
  }
  EXPECT_EQ(crc32c(zeros), 0x8a9136aau);
  EXPECT_EQ(crc32c(ones), 0x62a8ab43u);
  EXPECT_EQ(crc32c(up), 0x46dd794eu);
  EXPECT_EQ(crc32c(down), 0x113fdb5cu);
}

TEST(Crc32c, IncrementalMatchesWhole) {
  Bytes data = random_bytes(10000, 1);
  for (std::size_t split : {0u, 1u, 7u, 8u, 9u, 4095u, 9999u, 10000u}) {
    ByteView all(data);
    EXPECT_EQ(crc32c(all.subspan(split), crc32c(all.first(split))), crc32c(all)) << split;
  }
}

TEST(Crc32c, HardwareMatchesSoftware) {
  for (std::size_t n : {0u, 1u, 3u, 8u, 15u, 64u, 1000u, 65537u}) {
    Bytes data = random_bytes(n, n);
    // Unaligned starts exercise the byte-at-a-time edges of both paths.
    for (std::size_t start = 0; start < std::min<std::size_t>(n, 8); ++start) {
      ByteView v = ByteView(data).subspan(start);
      EXPECT_EQ(crc32c(v, 0x12345678), detail::crc32c_software(v, 0x12345678)) << n << "/" << start;
    }
  }
}

TEST(Xxh3, KnownVectors) {
  EXPECT_EQ(xxh3_64(ByteView()), 0x2d06800538d394c2ull);
  EXPECT_NE(xxh3_64(as_bytes("a")), xxh3_64(as_bytes("b")));
}

TEST(Blake3, KnownVectors) {
  EXPECT_EQ(to_hex(blake3(ByteView())),
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
  EXPECT_EQ(to_hex(blake3(as_bytes("abc"))),
            "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

TEST(Blake3, LargeInputUsesAllCodePaths) {
  // Big enough for the SIMD paths that hash many 1 KiB chunks at once.
  Bytes data = random_bytes(1 << 20, 7);
  Blake3Hash a = blake3(data);
  data[12345] ^= 1;
  EXPECT_NE(a, blake3(data));
}
