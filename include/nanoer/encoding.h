// Compact binary encoding for metadata: LEB128 variable-length integers,
// zigzag for signed values, and length-prefixed byte strings. Used by
// metadata log entries and checkpoints; see docs/format.md §8.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "nanoer/bytes.h"

namespace nanoer {

class Encoder {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void varint(std::uint64_t v) {
    if (v < 0x80) {
      buf_.push_back(static_cast<std::uint8_t>(v));  // most values fit in a byte
    } else {
      long_varint(v);
    }
  }
  void svarint(std::int64_t v);  // zigzag, so small negative numbers stay short
  void bytes(ByteView v);
  void str(std::string_view v) { bytes(as_bytes(v)); }
  void raw(ByteView v) { buf_.insert(buf_.end(), v.begin(), v.end()); }

  std::size_t size() const { return buf_.size(); }
  const Bytes& buffer() const { return buf_; }
  Bytes take() { return std::move(buf_); }
  void reserve(std::size_t n) { buf_.reserve(n); }

 private:
  void long_varint(std::uint64_t v);
  Bytes buf_;
};

// Every read is bounds-checked; malformed input throws Error(corrupt).
class Decoder {
 public:
  explicit Decoder(ByteView data) : data_(data) {}

  // Inline: metadata lookups decode an inode on almost every call.
  std::uint8_t u8() {
    if (pos_ >= data_.size()) fail("unexpected end");
    return data_[pos_++];
  }
  std::uint64_t varint() {
    if (pos_ < data_.size() && data_[pos_] < 0x80) return data_[pos_++];  // most values fit in a byte
    return long_varint();
  }
  std::int64_t svarint() {
    std::uint64_t v = varint();
    return static_cast<std::int64_t>((v >> 1) ^ (0 - (v & 1)));
  }
  // A length-prefixed byte string of at most `max` bytes.
  Bytes bytes(std::size_t max);
  std::string str(std::size_t max);
  ByteView raw(std::size_t n);

  bool done() const { return pos_ == data_.size(); }
  std::size_t remaining() const { return data_.size() - pos_; }

 private:
  std::uint64_t long_varint();
  [[noreturn]] void fail(const char* what) const;

  ByteView data_;
  std::size_t pos_ = 0;
};

}  // namespace nanoer
