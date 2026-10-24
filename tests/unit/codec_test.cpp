#include "nanoer/codec.h"

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "nanoer/error.h"

using namespace nanoer;

namespace {

constexpr CodecId kAll[] = {CodecId::none, CodecId::lz4, CodecId::zstd, CodecId::lzma};

Bytes random_bytes(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

Bytes text_like(std::size_t n) {
  static const std::string kWords[] = {"the ", "quick ", "filesystem ", "chunk ", "segment ",
                                       "compress ", "record ", "\n"};
  std::mt19937_64 rng(42);
  Bytes b;
  while (b.size() < n) {
    const std::string& w = kWords[rng() % 8];
    b.insert(b.end(), w.begin(), w.end());
  }
  b.resize(n);
  return b;
}

void expect_corrupt(CodecId codec, ByteView src, std::size_t size) {
  try {
    decompress(codec, src, size);
    FAIL() << codec_name(codec) << ": expected Error(corrupt)";
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::corrupt) << e.what();
  }
}

}  // namespace

TEST(Codec, NamesAndIdsRoundTrip) {
  for (CodecId c : kAll) {
    EXPECT_EQ(codec_from_name(codec_name(c)), c);
    EXPECT_EQ(codec_from_id(static_cast<std::uint8_t>(c)), c);
  }
  EXPECT_FALSE(codec_from_name("gzip"));
  EXPECT_FALSE(codec_from_id(4));
  // Ids are part of the on-disk format and must never change.
  EXPECT_EQ(static_cast<int>(CodecId::none), 0);
  EXPECT_EQ(static_cast<int>(CodecId::lz4), 1);
  EXPECT_EQ(static_cast<int>(CodecId::zstd), 2);
  EXPECT_EQ(static_cast<int>(CodecId::lzma), 3);
}

TEST(Codec, RoundTripAllCodecsSizesAndLevels) {
  for (CodecId c : kAll) {
    LevelRange r = codec_levels(c);
    for (int level : {r.min, r.fallback, r.max}) {
      if (c == CodecId::zstd && level == r.max) level = 19;  // 22 is very slow; 19 covers deep search
      for (std::size_t n : {0u, 1u, 100u, 4096u, 131072u}) {
        for (const Bytes& input : {Bytes(n, 0), text_like(n), random_bytes(n, n)}) {
          Bytes packed = compress(c, level, input);
          EXPECT_LE(packed.size(), compress_bound(c, input.size()));
          EXPECT_EQ(decompress(c, packed, input.size()), input)
              << codec_name(c) << " level " << level << " size " << n;
        }
      }
    }
  }
}

TEST(Codec, CompressibleDataShrinks) {
  Bytes input = text_like(128 << 10);
  for (CodecId c : {CodecId::lz4, CodecId::zstd, CodecId::lzma}) {
    EXPECT_LT(compress(c, codec_levels(c).fallback, input).size(), input.size() / 2) << codec_name(c);
  }
}

TEST(Codec, RejectsLevelsOutOfRange) {
  Bytes input(100, 1);
  for (CodecId c : kAll) {
    LevelRange r = codec_levels(c);
    EXPECT_THROW(compress(c, r.max + 1, input), Error);
    EXPECT_THROW(compress(c, r.min - 1, input), Error);
  }
}

TEST(Codec, WrongSizeIsCorrupt) {
  Bytes input = text_like(5000);
  for (CodecId c : kAll) {
    Bytes packed = compress(c, codec_levels(c).fallback, input);
    expect_corrupt(c, packed, input.size() - 1);
    expect_corrupt(c, packed, input.size() + 1);
  }
}

TEST(Codec, DamagedStreamsAreRejectedNotCrashing) {
  Bytes input = text_like(20000);
  for (CodecId c : {CodecId::lz4, CodecId::zstd, CodecId::lzma}) {
    Bytes packed = compress(c, codec_levels(c).fallback, input);
    expect_corrupt(c, ByteView(packed).first(packed.size() / 2), input.size());  // truncated
    expect_corrupt(c, random_bytes(packed.size(), 9), input.size());             // garbage
    Bytes trailing = packed;
    trailing.push_back(0);
    if (c != CodecId::lz4) expect_corrupt(c, trailing, input.size());  // LZ4 blocks have no end marker
  }
}

TEST(EncodeChunk, CompressesWhenItHelps) {
  Bytes input = text_like(64 << 10);
  EncodedChunk chunk = encode_chunk(input, CodecId::zstd, 3);
  EXPECT_EQ(chunk.codec, CodecId::zstd);
  EXPECT_LT(chunk.payload.size(), input.size());
  EXPECT_EQ(decode_chunk(chunk.codec, chunk.payload, chunk.uncompressed_size, chunk.xxh3), input);
}

TEST(EncodeChunk, StoresIncompressibleDataRaw) {
  Bytes input = random_bytes(64 << 10, 3);
  for (CodecId c : {CodecId::lz4, CodecId::zstd, CodecId::lzma}) {
    EncodedChunk chunk = encode_chunk(input, c, codec_levels(c).fallback);
    EXPECT_EQ(chunk.codec, CodecId::none) << codec_name(c);
    EXPECT_EQ(chunk.payload, input);
  }
  EXPECT_EQ(encode_chunk(ByteView(), CodecId::zstd, 1).codec, CodecId::none);
}

TEST(EncodeChunk, ChecksumMismatchIsCorrupt) {
  Bytes input = text_like(10000);
  EncodedChunk chunk = encode_chunk(input, CodecId::lz4, 0);
  EXPECT_THROW(decode_chunk(chunk.codec, chunk.payload, chunk.uncompressed_size, chunk.xxh3 ^ 1),
               Error);
}

TEST(Codec, ConcurrentUseFromManyThreads) {
  // Contexts are cached per thread; threads must never share one.
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([t, &failures] {
      for (int i = 0; i < 50; ++i) {
        Bytes input = text_like(static_cast<std::size_t>(1000 + t * 100 + i));
        for (CodecId c : {CodecId::lz4, CodecId::zstd, CodecId::lzma}) {
          EncodedChunk chunk = encode_chunk(input, c, c == CodecId::lzma ? 0 : 1);
          if (decode_chunk(chunk.codec, chunk.payload, chunk.uncompressed_size, chunk.xxh3) != input) {
            ++failures;
          }
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures, 0);
}
