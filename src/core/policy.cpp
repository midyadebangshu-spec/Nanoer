#include "nanoer/policy.h"

#include <algorithm>
#include <cstdint>

namespace nanoer {
namespace {

constexpr std::int64_t kDaySeconds = 24 * 60 * 60;

}  // namespace

std::string_view policy_name(Policy policy) {
  switch (policy) {
    case Policy::none: return "none";
    case Policy::fast: return "fast";
    case Policy::balanced: return "balanced";
    case Policy::max: return "max";
  }
  return "balanced";
}

std::optional<Policy> parse_policy(std::string_view value) {
  for (Policy p : {Policy::none, Policy::fast, Policy::balanced, Policy::max}) {
    if (value == policy_name(p)) return p;
  }
  return std::nullopt;
}

Policy policy_of(const Inode& inode) {
  auto it = inode.xattrs.find(kPolicyXattr);
  if (it == inode.xattrs.end()) return Policy::balanced;
  std::string_view value(reinterpret_cast<const char*>(it->second.data()), it->second.size());
  return parse_policy(value).value_or(Policy::balanced);
}

StorageForm hot_form(Policy policy, const Superblock& sb) {
  switch (policy) {
    case Policy::none: return {CodecId::none, 0};
    case Policy::fast: return {CodecId::lz4, 0};
    case Policy::balanced: break;
    case Policy::max: return {CodecId::zstd, 3};
  }
  return {sb.default_codec, sb.default_level};
}

// Measured on /usr (KNOWLEDGE.md §14): zstd 9 in 1 MiB units stores 14 %
// less than zstd 1 in 128 KiB chunks and decompresses as fast; zstd 19 two
// points less again, but compresses at 3 MB/s.
std::optional<StorageForm> cold_form(Policy policy) {
  switch (policy) {
    case Policy::none:
    case Policy::fast: return std::nullopt;
    case Policy::balanced: return StorageForm{CodecId::zstd, 9};
    case Policy::max: return StorageForm{CodecId::zstd, 19};
  }
  return std::nullopt;
}

std::optional<std::int64_t> cold_after_s(Policy policy, std::int64_t setting) {
  if (!cold_form(policy) || setting < 0) return std::nullopt;
  return policy == Policy::max ? std::min(setting, kDaySeconds) : setting;
}

std::optional<std::int64_t> parse_duration(std::string_view text) {
  if (text == "never") return -1;
  std::int64_t n = 0;
  std::size_t i = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    if (n > INT64_MAX / 1'000'000) return std::nullopt;  // far beyond any useful age
    n = n * 10 + (text[i] - '0');
  }
  if (i == 0) return std::nullopt;
  std::string_view unit = text.substr(i);
  if (unit.empty() || unit == "s") return n;
  if (unit == "m") return n * 60;
  if (unit == "h") return n * 60 * 60;
  if (unit == "d") return n * kDaySeconds;
  return std::nullopt;
}

}  // namespace nanoer
