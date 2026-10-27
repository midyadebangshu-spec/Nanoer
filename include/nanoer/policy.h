// Folder policies (docs/M5_PLAN.md §4): how a file's data is stored, chosen
// with the extended attribute user.nanoer.policy on the file or a folder it
// was created in (new files and folders receive a copy of their parent's).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "nanoer/codec.h"
#include "nanoer/format.h"
#include "nanoer/metadata.h"

namespace nanoer {

inline constexpr const char* kPolicyXattr = "user.nanoer.policy";

enum class Policy : std::uint8_t {
  none,      // stored raw, never recompressed
  fast,      // LZ4, never recompressed (reads fastest)
  balanced,  // the default: the filesystem's codec, then zstd 9 in cold units
  max,       // zstd 3, then zstd 19 in cold units, after at most a day
};

std::string_view policy_name(Policy policy);
// Exactly one of the names above; anything else is not a policy.
std::optional<Policy> parse_policy(std::string_view value);

// The file's own attribute, or the default.
Policy policy_of(const Inode& inode);

struct StorageForm {
  CodecId codec;
  int level;
};

// New data.
StorageForm hot_form(Policy policy, const Superblock& sb);
// Data that has gone cold; nothing for policies that never recompress.
std::optional<StorageForm> cold_form(Policy policy);
// How long a file must go unchanged before its data is cold, given the
// filesystem's setting for the default policy (seconds); nothing if never.
std::optional<std::int64_t> cold_after_s(Policy policy, std::int64_t setting);

// A duration such as "7d", "12h", "30m", "90s" or "3600" (seconds), or
// "never" (-1).
std::optional<std::int64_t> parse_duration(std::string_view text);

}  // namespace nanoer
