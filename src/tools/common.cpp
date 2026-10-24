#include "common.h"

#include <bit>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace nanoer::tools {

std::optional<std::uint64_t> parse_size(std::string_view text) {
  std::uint64_t number = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
  if (ec != std::errc() || end == text.data()) return std::nullopt;
  std::string unit(end, text.data() + text.size());
  for (char& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (unit.ends_with("ib")) unit.resize(unit.size() - 2);
  else if (unit.size() == 2 && unit.back() == 'b') unit.pop_back();

  unsigned shift;
  if (unit.empty() || unit == "b") shift = 0;
  else if (unit == "k") shift = 10;
  else if (unit == "m") shift = 20;
  else if (unit == "g") shift = 30;
  else if (unit == "t") shift = 40;
  else return std::nullopt;
  if (shift && number > (UINT64_MAX >> shift)) return std::nullopt;
  return number << shift;
}

std::string format_size(std::uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024 && unit < 4) {
    value /= 1024;
    ++unit;
  }
  char buf[32];
  if (unit == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
  else std::snprintf(buf, sizeof buf, "%.1f %s", value, kUnits[unit]);
  return buf;
}

std::optional<unsigned> exact_log2(std::uint64_t value) {
  if (!std::has_single_bit(value)) return std::nullopt;
  return static_cast<unsigned>(std::countr_zero(value));
}

std::string format_time_ns(std::uint64_t ns) {
  std::time_t seconds = static_cast<std::time_t>(ns / 1'000'000'000);
  std::tm tm;
  gmtime_r(&seconds, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
  return buf;
}

int fail(std::string_view program, std::string_view message) {
  std::cerr << program << ": " << message << "\n";
  return 1;
}

}  // namespace nanoer::tools
