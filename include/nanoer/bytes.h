// Byte buffers and little-endian encoding helpers.
//
// Every multi-byte integer in the on-disk format is little-endian. These
// helpers encode byte by byte, so the result is identical on any host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanoer {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
using MutableByteView = std::span<std::uint8_t>;

inline ByteView as_bytes(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

template <typename T>
inline T load_le(const std::uint8_t* p) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<T>(p[i]) << (8 * i));
  }
  return value;
}

template <typename T>
inline void store_le(std::uint8_t* p, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    p[i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

std::string to_hex(ByteView data);

}  // namespace nanoer
