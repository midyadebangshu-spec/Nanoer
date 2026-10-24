#include "nanoer/encoding.h"

#include "nanoer/error.h"

namespace nanoer {

void Encoder::varint(std::uint64_t v) {
  while (v >= 0x80) {
    buf_.push_back(static_cast<std::uint8_t>(v | 0x80));
    v >>= 7;
  }
  buf_.push_back(static_cast<std::uint8_t>(v));
}

void Encoder::svarint(std::int64_t v) {
  varint((static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63));
}

void Encoder::bytes(ByteView v) {
  varint(v.size());
  raw(v);
}

void Decoder::fail(const char* what) const {
  throw Error(Errc::corrupt, std::string("metadata decoding: ") + what + " at byte " +
                                 std::to_string(pos_));
}

std::uint8_t Decoder::u8() {
  if (pos_ >= data_.size()) fail("unexpected end");
  return data_[pos_++];
}

std::uint64_t Decoder::varint() {
  std::uint64_t v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    std::uint8_t b = u8();
    if (shift == 63 && b > 1) fail("integer overflow");
    v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
  }
  fail("integer too long");
}

std::int64_t Decoder::svarint() {
  std::uint64_t v = varint();
  return static_cast<std::int64_t>((v >> 1) ^ (0 - (v & 1)));
}

ByteView Decoder::raw(std::size_t n) {
  if (n > remaining()) fail("unexpected end");
  ByteView v = data_.subspan(pos_, n);
  pos_ += n;
  return v;
}

Bytes Decoder::bytes(std::size_t max) {
  std::uint64_t n = varint();
  if (n > max) fail("string too long");
  ByteView v = raw(static_cast<std::size_t>(n));
  return Bytes(v.begin(), v.end());
}

std::string Decoder::str(std::size_t max) {
  std::uint64_t n = varint();
  if (n > max) fail("string too long");
  ByteView v = raw(static_cast<std::size_t>(n));
  return std::string(v.begin(), v.end());
}

}  // namespace nanoer
