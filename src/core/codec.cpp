#include "nanoer/codec.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <lz4.h>
#include <lz4hc.h>
#include <lzma.h>
#include <zstd.h>

#include "nanoer/checksum.h"
#include "nanoer/error.h"

namespace nanoer {
namespace {

constexpr int kLz4HcMinLevel = 3;  // levels below this use the fast LZ4 compressor

// liblzma refuses inputs needing more decoder memory than this. Nanoer's own
// chunks need far less (the dictionary never exceeds the chunk size), so
// this only stops damaged or hostile data from causing huge allocations.
constexpr std::uint64_t kLzmaDecodeMemoryLimit = 128u << 20;

// An empty span may have a null data() pointer, and some libraries (LZ4's
// HC compressor) crash on null even with length 0. Hand them a real address.
const std::uint8_t* in_ptr(ByteView v) {
  static const std::uint8_t kEmpty = 0;
  return v.empty() ? &kEmpty : v.data();
}

std::uint8_t* out_ptr(MutableByteView v) {
  thread_local std::uint8_t empty = 0;
  return v.empty() ? &empty : v.data();
}

[[noreturn]] void corrupt(CodecId codec, const std::string& what) {
  throw Error(Errc::corrupt, std::string(codec_name(codec)) + ": " + what);
}

void check_level(CodecId codec, int level) {
  LevelRange r = codec_levels(codec);
  if (level < r.min || level > r.max) {
    throw Error(Errc::invalid_argument,
                std::string(codec_name(codec)) + " level " + std::to_string(level) +
                    " is outside " + std::to_string(r.min) + ".." + std::to_string(r.max));
  }
}

// zstd contexts are reused per thread: creating one costs far more than
// compressing a small chunk.
struct ZstdCctxDeleter {
  void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct ZstdDctxDeleter {
  void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); }
};

ZSTD_CCtx* zstd_cctx() {
  thread_local std::unique_ptr<ZSTD_CCtx, ZstdCctxDeleter> ctx(ZSTD_createCCtx());
  if (!ctx) throw std::bad_alloc();
  return ctx.get();
}

ZSTD_DCtx* zstd_dctx() {
  thread_local std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter> ctx(ZSTD_createDCtx());
  if (!ctx) throw std::bad_alloc();
  return ctx.get();
}

int lz4_size(std::size_t n) {
  if (n > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
    throw Error(Errc::invalid_argument, "lz4: input too large");
  }
  return static_cast<int>(n);
}

std::size_t lzma_compress(int level, ByteView src, MutableByteView dst) {
  lzma_options_lzma options;
  if (lzma_lzma_preset(&options, static_cast<std::uint32_t>(level))) {
    throw Error(Errc::invalid_argument, "lzma: bad preset");
  }
  // A dictionary larger than the input cannot find more matches; it only
  // costs memory and set-up time on every chunk (8 MiB at the default
  // preset). Shrink it to the input size.
  std::uint64_t wanted = std::max<std::uint64_t>(LZMA_DICT_SIZE_MIN, std::bit_ceil(src.size()));
  options.dict_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.dict_size, wanted));
  lzma_filter filters[] = {{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}};
  std::size_t out_pos = 0;
  lzma_ret ret = lzma_stream_buffer_encode(filters, LZMA_CHECK_NONE, nullptr, in_ptr(src),
                                           src.size(), out_ptr(dst), &out_pos, dst.size());
  if (ret != LZMA_OK) throw Error(Errc::io, "lzma: compression failed (" + std::to_string(ret) + ")");
  return out_pos;
}

void lzma_decompress(ByteView src, MutableByteView dst) {
  std::uint64_t memlimit = kLzmaDecodeMemoryLimit;
  std::size_t in_pos = 0;
  std::size_t out_pos = 0;
  lzma_ret ret = lzma_stream_buffer_decode(&memlimit, 0, nullptr, in_ptr(src), &in_pos, src.size(),
                                           out_ptr(dst), &out_pos, dst.size());
  if (ret != LZMA_OK) corrupt(CodecId::lzma, "invalid stream (" + std::to_string(ret) + ")");
  if (in_pos != src.size()) corrupt(CodecId::lzma, "trailing bytes after the stream");
  if (out_pos != dst.size()) corrupt(CodecId::lzma, "decoded to the wrong size");
}

}  // namespace

std::string_view codec_name(CodecId codec) {
  switch (codec) {
    case CodecId::none: return "none";
    case CodecId::lz4: return "lz4";
    case CodecId::zstd: return "zstd";
    case CodecId::lzma: return "lzma";
  }
  return "unknown";
}

std::optional<CodecId> codec_from_name(std::string_view name) {
  for (CodecId c : {CodecId::none, CodecId::lz4, CodecId::zstd, CodecId::lzma}) {
    if (codec_name(c) == name) return c;
  }
  return std::nullopt;
}

std::optional<CodecId> codec_from_id(std::uint8_t id) {
  if (id <= static_cast<std::uint8_t>(CodecId::lzma)) return static_cast<CodecId>(id);
  return std::nullopt;
}

LevelRange codec_levels(CodecId codec) {
  switch (codec) {
    case CodecId::none: return {0, 0, 0};
    case CodecId::lz4: return {0, LZ4HC_CLEVEL_MAX, 0};
    case CodecId::zstd: return {-7, ZSTD_maxCLevel(), 3};
    case CodecId::lzma: return {0, 9, 6};
  }
  return {0, 0, 0};
}

std::size_t compress_bound(CodecId codec, std::size_t size) {
  switch (codec) {
    case CodecId::none: return size;
    case CodecId::lz4: return static_cast<std::size_t>(LZ4_compressBound(lz4_size(size)));
    case CodecId::zstd: return ZSTD_compressBound(size);
    case CodecId::lzma: return lzma_stream_buffer_bound(size);
  }
  throw Error(Errc::invalid_argument, "unknown codec");
}

std::size_t compress(CodecId codec, int level, ByteView src, MutableByteView dst) {
  check_level(codec, level);
  if (dst.size() < compress_bound(codec, src.size())) {
    throw Error(Errc::invalid_argument, "compress: output buffer smaller than compress_bound()");
  }
  switch (codec) {
    case CodecId::none:
      if (!src.empty()) std::memcpy(dst.data(), src.data(), src.size());
      return src.size();
    case CodecId::lz4: {
      auto* in = reinterpret_cast<const char*>(in_ptr(src));
      auto* out = reinterpret_cast<char*>(out_ptr(dst));
      int cap = static_cast<int>(std::min<std::size_t>(dst.size(), std::numeric_limits<int>::max()));
      int n = level < kLz4HcMinLevel
                  ? LZ4_compress_default(in, out, lz4_size(src.size()), cap)
                  : LZ4_compress_HC(in, out, lz4_size(src.size()), cap, level);
      if (n <= 0 && !src.empty()) throw Error(Errc::io, "lz4: compression failed");
      return static_cast<std::size_t>(n);
    }
    case CodecId::zstd: {
      std::size_t n = ZSTD_compressCCtx(zstd_cctx(), out_ptr(dst), dst.size(), in_ptr(src),
                                        src.size(), level);
      if (ZSTD_isError(n)) throw Error(Errc::io, std::string("zstd: ") + ZSTD_getErrorName(n));
      return n;
    }
    case CodecId::lzma:
      return lzma_compress(level, src, dst);
  }
  throw Error(Errc::invalid_argument, "unknown codec");
}

Bytes compress(CodecId codec, int level, ByteView src) {
  Bytes out(compress_bound(codec, src.size()));
  out.resize(compress(codec, level, src, out));
  return out;
}

void decompress(CodecId codec, ByteView src, MutableByteView dst) {
  switch (codec) {
    case CodecId::none:
      if (src.size() != dst.size()) corrupt(codec, "stored size does not match");
      if (!src.empty()) std::memcpy(dst.data(), src.data(), src.size());
      return;
    case CodecId::lz4: {
      if (src.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
          dst.size() > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
        corrupt(codec, "sizes out of range");
      }
      int n = LZ4_decompress_safe(reinterpret_cast<const char*>(in_ptr(src)),
                                  reinterpret_cast<char*>(out_ptr(dst)), static_cast<int>(src.size()),
                                  static_cast<int>(dst.size()));
      if (n < 0) corrupt(codec, "invalid stream");
      if (static_cast<std::size_t>(n) != dst.size()) corrupt(codec, "decoded to the wrong size");
      return;
    }
    case CodecId::zstd: {
      std::size_t n =
          ZSTD_decompressDCtx(zstd_dctx(), out_ptr(dst), dst.size(), in_ptr(src), src.size());
      if (ZSTD_isError(n)) corrupt(codec, ZSTD_getErrorName(n));
      if (n != dst.size()) corrupt(codec, "decoded to the wrong size");
      return;
    }
    case CodecId::lzma:
      lzma_decompress(src, dst);
      return;
  }
  corrupt(codec, "unknown codec");
}

Bytes decompress(CodecId codec, ByteView src, std::size_t uncompressed_size) {
  Bytes out(uncompressed_size);
  decompress(codec, src, out);
  return out;
}

EncodedChunk encode_chunk(ByteView data, CodecId codec, int level) {
  if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw Error(Errc::invalid_argument, "chunk larger than 4 GiB");
  }
  EncodedChunk chunk{CodecId::none, static_cast<std::uint32_t>(data.size()), xxh3_64(data), {}};
  if (codec != CodecId::none && !data.empty()) {
    Bytes packed = compress(codec, level, data);
    // Already-compressed data (media, archives) usually does not shrink.
    // Storing it raw avoids growth and makes reading it a plain copy.
    if (packed.size() < data.size()) {
      chunk.codec = codec;
      chunk.payload = std::move(packed);
      return chunk;
    }
  }
  chunk.payload.assign(data.begin(), data.end());
  return chunk;
}

Bytes decode_chunk(CodecId codec, ByteView payload, std::uint32_t uncompressed_size,
                   std::uint64_t xxh3) {
  Bytes data = decompress(codec, payload, uncompressed_size);
  if (xxh3_64(data) != xxh3) {
    throw Error(Errc::corrupt, "chunk contents do not match their checksum");
  }
  return data;
}

}  // namespace nanoer
