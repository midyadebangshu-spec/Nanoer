// Encodings shared by the operations, the legacy metadata and the tree
// items (docs/format.md §8, §9). Internal to the engine.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nanoer/encoding.h"
#include "nanoer/ops.h"

namespace nanoer::meta {

[[noreturn]] void corrupt(const std::string& what);
std::uint32_t u32(std::uint64_t v);
bool valid_name(std::string_view name);
void encode_attrs(Encoder& out, const InodeAttrs& a);
InodeAttrs decode_attrs(Decoder& in);
void encode_ref(Encoder& out, const ChunkRef& r);
ChunkRef decode_ref(Decoder& in);
// The extended form (flags first); `owner` only in SetChunkRef.
void encode_ref_ex(Encoder& out, const ChunkRef& r, bool share,
                   const std::optional<ChunkUser>& owner = std::nullopt);
ChunkRef decode_ref_ex(Decoder& in, bool& share, std::optional<ChunkUser>* owner = nullptr);

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace nanoer::meta
