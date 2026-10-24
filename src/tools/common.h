// Helpers shared by the command-line tools.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nanoer::tools {

// Parses sizes such as "64M", "128KiB", "20G" or "4096" (binary units).
std::optional<std::uint64_t> parse_size(std::string_view text);

// Formats a byte count as e.g. "64.0 MiB".
std::string format_size(std::uint64_t bytes);

// log2 of `value` if it is a power of two.
std::optional<unsigned> exact_log2(std::uint64_t value);

std::string format_time_ns(std::uint64_t ns);

// Prints "<program>: <message>" to stderr and returns exit status 1.
int fail(std::string_view program, std::string_view message);

}  // namespace nanoer::tools
