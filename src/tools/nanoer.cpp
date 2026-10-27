// nanoer: inspect Nanoer filesystems and measure codecs.

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/xattr.h>

#include "common.h"
#include "nanoer/check.h"
#include "nanoer/checksum.h"
#include "nanoer/codec.h"
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/segment.h"

using namespace nanoer;
using namespace nanoer::tools;

namespace {

constexpr std::string_view kProgram = "nanoer";

void usage(std::ostream& out) {
  out << "Usage: nanoer COMMAND [options]\n"
         "\n"
         "Commands:\n"
         "  info TARGET               describe the filesystem on an image file or block device\n"
         "  bench [options] PATH...   measure every codec on real files (directories are walked)\n"
         "      -c, --chunk-size SIZE   chunk size (default 128K)\n"
         "      -m, --max-size SIZE     read at most this much data (default 64M)\n"
         "      -p, --per-file SIZE     read at most this much from each file, so the sample\n"
         "                              spans many files (default 4M)\n"
         "  fsck [options] TARGET     check the filesystem (it must not be mounted)\n"
         "      --repair, -y, -a, -p    repair what is safe to repair\n"
         "      --deep                  also read and verify every data chunk\n"
         "      -n                      check only (the default)\n"
         "  gc [options] TARGET       compact the filesystem (it must not be mounted): copy the\n"
         "                            live data out of mostly empty segments and free them\n"
         "      --max-full PERCENT      collect segments at most this full (default 90)\n"
         "  tier [--age AGE] TARGET   recompress cold data now (it must not be mounted): data\n"
         "                            of files unchanged for AGE (default 7d; 0: everything)\n"
         "                            in its cold form, which is smaller and reads as fast\n"
         "  policy [-r] PATH [POLICY] show or set how a folder's (or file's) data is stored,\n"
         "                            on a mounted filesystem: none, fast, balanced (the\n"
         "                            default) or max. New files and folders inherit it\n"
         "      -r, --recursive         also set it on everything already inside\n"
         "  stats [--codecs] TARGET   where the space goes: what holes, dedup and compression\n"
         "                            saved; segments, GC, memory. TARGET: an image or device\n"
         "                            (not mounted), or the mount point of a mounted filesystem\n"
         "      --codecs                also break stored data down by codec (reads every\n"
         "                              record header; not for a mount point)\n"
         "  version                   print the version\n"
         "\n"
         "fsck exit codes: 0 clean, 1 errors fixed, 4 errors left, 8 could not run.\n"
         "Also installed as fsck.nanoer, for fsck -t nanoer and boot-time checks.\n";
}

std::string features(std::uint64_t f, std::initializer_list<std::pair<std::uint64_t, const char*>> names = {}) {
  if (f == 0) return "none";
  std::string out;
  for (const auto& [bit, name] : names) {
    if (!(f & bit)) continue;
    out += (out.empty() ? "" : " + ") + std::string(name);
    f &= ~bit;
  }
  if (f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(f));
    out += (out.empty() ? "" : " + ") + std::string(buf);
  }
  return out;
}

const char* state_name(DecodedSuperblock::State s) {
  switch (s) {
    case DecodedSuperblock::State::invalid: return "invalid";
    case DecodedSuperblock::State::unsupported: return "unsupported";
    case DecodedSuperblock::State::valid: return "valid";
  }
  return "?";
}

int cmd_info(const std::vector<std::string_view>& args) {
  if (args.size() != 1) return fail(kProgram, "info needs exactly one TARGET");
  std::string target(args[0]);
  auto store = FileStore::open(target, FileStore::Mode::read_only);

  std::cout << target << (store->is_block_device() ? " (block device, " : " (image file, ")
            << format_size(store->size()) << ")\n\nSuperblock slots:\n";
  for (int i = 0; i < 2; ++i) {
    DecodedSuperblock d = decode_superblock(store->read(kSuperblockOffsets[i], kBlockSize));
    std::cout << "  slot " << i << ": " << state_name(d.state);
    if (d.state != DecodedSuperblock::State::invalid) std::cout << ", generation " << d.sb.generation;
    if (!d.reason.empty()) std::cout << " (" << d.reason << ")";
    std::cout << "\n";
  }

  LoadedSuperblock loaded = load_superblock(*store);
  const Superblock& sb = loaded.sb;
  std::uint64_t used = 0;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    if (read_segment_header(*store, sb, i)) ++used;
  }

  std::cout << "\nFilesystem (from slot " << loaded.slot << "):\n"
            << "  format:          " << sb.version_major << "." << sb.version_minor << "\n"
            << "  UUID:            " << uuid_to_string(sb.uuid) << "\n"
            << "  label:           " << (sb.label.empty() ? "(none)" : sb.label) << "\n"
            << "  size:            " << format_size(sb.store_size) << "\n"
            << "  segments:        " << sb.segment_count << " x " << format_size(sb.segment_size())
            << ", " << used << " in use\n"
            << "  chunk size:      " << format_size(sb.chunk_size()) << "\n"
            << "  new data:        " << codec_name(sb.default_codec) << " level "
            << int{sb.default_level} << "\n"
            << "  features:        compat " << features(sb.features_compat, {{compat_features::no_dedup, "no-dedup"}})
            << ", ro_compat "
            << features(sb.features_ro_compat) << ", incompat " << features(sb.features_incompat, {{incompat_features::shared_records, "shared-records"},
                                                                      {incompat_features::cold_data, "cold-data"}})
            << (loaded.read_only ? " (read-only for this version)" : "") << "\n"
            << "  state:           "
            << (sb.state & state_flags::errors ? "damage was found; run nanoer fsck" : "clean") << "\n"
            << "  checkpoint:      "
            << (sb.checkpoint_offset ? "at byte " + std::to_string(sb.checkpoint_offset) : "none")
            << "\n"
            << "  next segment:    seq " << sb.next_segment_seq << "\n"
            << "  created:         " << format_time_ns(sb.created_ns) << "\n"
            << "  updated:         " << format_time_ns(sb.updated_ns) << "\n"
            << "\nCRC-32C hardware acceleration: " << (crc32c_hardware_accelerated() ? "yes" : "no")
            << "\n";

  // The contents, from a read-only mount (replays the log, writes nothing).
  Filesystem::Options options;
  options.read_only = true;
  options.start_threads = false;
  options.safe_space = 0;
  std::unique_ptr<Filesystem> fs = Filesystem::mount(*store, options);
  FsInfo i = fs->info();
  double ratio = i.logical_bytes ? 100.0 * static_cast<double>(i.stored_bytes) /
                                       static_cast<double>(i.logical_bytes)
                                 : 0.0;
  char stored[64];
  std::snprintf(stored, sizeof stored, "%s (%.1f%% of logical)", format_size(i.stored_bytes).c_str(),
                ratio);
  std::cout << "\nContents:\n"
            << "  files:           " << i.files << "\n"
            << "  directories:     " << i.directories << "\n"
            << "  other:           " << i.other_inodes << " (symlinks, devices, FIFOs, sockets)\n"
            << "  logical size:    " << format_size(i.logical_bytes) << "\n"
            << "  stored data:     " << stored << "\n"
            << "  segments:        " << i.segments_in_use << " in use, " << i.segments_free
            << " free\n"
            << "  checkpoint:      " << format_size(i.checkpoint_bytes) << ", transaction "
            << (i.last_txid - i.replayed_transactions) << "\n"
            << "  log after it:    " << i.replayed_transactions << " transactions\n";
  if (i.log_damaged) std::cout << "  log:             DAMAGED; later changes cannot be read (run nanoer fsck)\n";
  if (i.checkpoint_fallback) std::cout << "  checkpoint:      the latest is damaged; read the previous one\n";
  return 0;
}

// ---- fsck ------------------------------------------------------------------

int cmd_fsck(const std::vector<std::string_view>& args) {
  FsckOptions options;
  std::string target;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "-h" || a == "--help") {
      usage(std::cout);
      return 0;
    } else if (a == "--repair" || a == "-y" || a == "-a" || a == "-p") {
      options.repair = true;
    } else if (a == "--deep") {
      options.deep = true;
    } else if (a == "-n") {
      options.repair = false;
    } else if (a == "-f" || a == "-V" || a == "-t" || a == "-T" || a == "-M") {
      // fsck(8) options with nothing to do here (Nanoer always checks fully).
    } else if (a == "-C") {
      if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') ++i;  // progress fd
    } else if (a.starts_with("-C")) {
    } else if (!a.empty() && a[0] == '-') {
      return fail(kProgram, "unknown fsck option " + std::string(a)) * 8;
    } else {
      target = std::string(a);
    }
  }
  if (target.empty()) return fail(kProgram, "fsck needs a TARGET") * 8;

  FsckReport report;
  try {
    // A store that is mounted is locked: fsck refuses to touch it.
    auto store = FileStore::open(target, options.repair ? FileStore::Mode::read_write
                                                        : FileStore::Mode::read_only);
    report = fsck(*store, options);
    if (options.repair && !report.fixed.empty()) {
      // Check the result again: repairs must leave a clean filesystem.
      FsckReport after = fsck(*store, {.repair = false, .deep = options.deep});
      for (std::string& e : after.errors) report.errors.push_back("after repair: " + e);
    }
  } catch (const Error& e) {
    std::cerr << "nanoer fsck: " << e.what() << "\n";
    return 8;
  }

  auto section = [](const char* title, const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::cout << title << ":\n";
    for (const std::string& l : lines) std::cout << "  " << l << "\n";
  };
  section("Errors", report.errors);
  section("Fixed", report.fixed);
  section("Lost", report.lost);
  section("Notes", report.notes);
  std::cout << target << ": " << report.files << " files, " << report.directories
            << " directories, " << report.chunks << " chunks";
  if (options.deep) std::cout << " (" << report.chunks_verified << " verified)";
  std::cout << ": "
            << (report.exit_code() == 0   ? "clean"
                : report.exit_code() == 1 ? "repaired"
                                          : "errors remain")
            << "\n";
  if (report.exit_code() == 4 && !options.repair) std::cout << "Run with --repair to fix them.\n";
  return report.exit_code();
}

// ---- gc --------------------------------------------------------------------

int cmd_gc(const std::vector<std::string_view>& args) {
  double max_full = 0.9;
  std::string target;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--max-full" && i + 1 < args.size()) {
      int percent = std::atoi(std::string(args[++i]).c_str());
      if (percent < 1 || percent > 97) return fail(kProgram, "--max-full must be 1 to 97");
      max_full = percent / 100.0;
    } else if (!args[i].empty() && args[i][0] == '-') {
      return fail(kProgram, "unknown gc option " + std::string(args[i]));
    } else {
      target = std::string(args[i]);
    }
  }
  if (target.empty()) return fail(kProgram, "gc needs a TARGET");
  auto store = FileStore::open(target, FileStore::Mode::read_write);
  Filesystem::Options options;
  options.start_threads = false;
  auto fs = Filesystem::mount(*store, options);
  if (fs->read_only()) return fail(kProgram, "the filesystem can only be mounted read-only; run nanoer fsck");
  FsInfo before = fs->info();
  std::uint64_t freed = fs->collect_garbage(max_full);
  FsInfo after = fs->info();
  fs->unmount();
  std::cout << target << ": copied " << format_size(after.gc.bytes_copied) << " in "
            << after.gc.records_copied << " records; " << after.gc.segments_emptied << " segments emptied, "
            << freed << " more free (" << before.segments_free << " -> " << after.segments_free << " of "
            << fs->superblock().segment_count << ")\n";
  if (after.gc.damaged) {
    std::cout << after.gc.damaged << " damaged records were left in place; run nanoer fsck\n";
    return 1;
  }
  return 0;
}

int cmd_policy(const std::vector<std::string_view>& args) {
  bool recursive = false;
  std::vector<std::string> positional;
  for (std::string_view a : args) {
    if (a == "-r" || a == "--recursive") {
      recursive = true;
    } else if (!a.empty() && a[0] == '-') {
      return fail(kProgram, "unknown policy option " + std::string(a));
    } else {
      positional.emplace_back(a);
    }
  }
  if (positional.empty() || positional.size() > 2) return fail(kProgram, "policy needs PATH [POLICY]");
  const std::string& path = positional[0];
  if (positional.size() == 1) {
    if (recursive) return fail(kProgram, "--recursive needs a POLICY to set");
    char buf[64];
    ssize_t n = getxattr(path.c_str(), kPolicyXattr, buf, sizeof buf);
    if (n < 0 && errno == ENODATA) {
      std::cout << path << ": balanced (the default)\n";
      return 0;
    }
    if (n < 0) return fail(kProgram, path + ": " + std::strerror(errno));
    std::cout << path << ": " << std::string_view(buf, static_cast<std::size_t>(n)) << "\n";
    return 0;
  }
  const std::string& value = positional[1];
  if (!parse_policy(value)) return fail(kProgram, "unknown policy " + value + " (none, fast, balanced or max)");
  std::uint64_t count = 0;
  auto set = [&](const std::string& p) {
    if (setxattr(p.c_str(), kPolicyXattr, value.data(), value.size(), 0) != 0) {
      if (errno == ENOTSUP || errno == EPERM) {
        throw Error(Errc::invalid_argument, p + ": not on a Nanoer filesystem (or too old a one)");
      }
      throw Error(Errc::posix, p + ": " + std::strerror(errno));
    }
    ++count;
  };
  set(path);
  if (recursive && std::filesystem::is_directory(path)) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(path)) {
      // (Symlinks hold no data, and other files no extended attributes.)
      if (e.is_symlink() || !(e.is_regular_file() || e.is_directory())) continue;
      set(e.path().string());
    }
  }
  std::cout << path << ": " << value;
  if (recursive) std::cout << " (" << count << " files and folders)";
  std::cout << "\n";
  return 0;
}

int cmd_tier(const std::vector<std::string_view>& args) {
  std::optional<std::int64_t> age;
  std::string target;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--age" && i + 1 < args.size()) {
      age = parse_duration(args[++i]);
      if (!age || *age < 0) return fail(kProgram, "--age needs a duration such as 7d, 12h or 0");
    } else if (!args[i].empty() && args[i][0] == '-') {
      return fail(kProgram, "unknown tier option " + std::string(args[i]));
    } else {
      target = std::string(args[i]);
    }
  }
  if (target.empty()) return fail(kProgram, "tier needs a TARGET");
  auto store = FileStore::open(target, FileStore::Mode::read_write);
  Filesystem::Options options;
  options.start_threads = false;
  auto fs = Filesystem::mount(*store, options);
  if (fs->read_only()) return fail(kProgram, "the filesystem can only be mounted read-only; run nanoer fsck");
  fs->start();  // compression threads
  FsInfo before = fs->info();
  auto t0 = std::chrono::steady_clock::now();
  std::uint64_t chunks = fs->recompress_cold(age);
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  FsInfo after = fs->info();
  fs->unmount();
  const TierStats& t = after.tier;
  std::cout << target << ": " << chunks << " chunks made cold in " << std::fixed << std::setprecision(1) << seconds
            << " s: " << t.singles << " records, " << t.packs
            << " packs, " << t.in_place << " left as they were (no smaller)\n"
            << "  rewritten: " << format_size(t.stored_before) << " -> " << format_size(t.stored_after)
            << "; stored data " << format_size(before.stored_bytes) << " -> " << format_size(after.stored_bytes)
            << " (" << format_size(t.bytes_read) << " examined)\n";
  if (t.damaged) {
    std::cout << t.damaged << " damaged records were left in place; run nanoer fsck\n";
    return 1;
  }
  return 0;
}

// ---- stats -----------------------------------------------------------------

void print_stats(const std::string& target, const SpaceStats& s) {
  auto pct = [](std::uint64_t part, std::uint64_t whole) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f%%", whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0);
    return std::string(buf);
  };
  auto minus = [&](std::uint64_t v) { return "-" + format_size(v) + " (" + pct(v, s.logical_bytes) + ")"; };
  std::int64_t compression = static_cast<std::int64_t>(s.unique_bytes) - static_cast<std::int64_t>(s.stored_bytes);
  std::cout << target << ": " << s.files << " files, " << s.directories << " directories, " << s.other_inodes
            << " other\n\n"
            << "Contents:\n"
            << "  logical size:      " << format_size(s.logical_bytes) << "\n"
            << "  holes and zeros:   " << minus(s.saved_by_holes()) << "\n"
            << "  dedup:             " << minus(s.dedup_bytes) << ", " << s.shared_records << " records shared\n"
            << "  compression:       "
            << (compression >= 0 ? minus(static_cast<std::uint64_t>(compression))
                                 : "+" + format_size(static_cast<std::uint64_t>(-compression)) + " (headers)")
            << ", " << s.packed_files << " small files in " << s.packs << " packs\n";
  if (s.pack_dead_bytes) std::cout << "  dead in packs:     +" << format_size(s.pack_dead_bytes) << "\n";
  std::cout << "  stored:            " << format_size(s.stored_bytes) << " (" << pct(s.stored_bytes, s.logical_bytes)
            << " of logical) in " << s.records << " records\n";
  std::cout << "  cold:              " << format_size(s.cold_bytes) << " of data in its cold form, stored in "
            << format_size(s.cold_stored_bytes) << "\n";
  for (const auto& [policy, p] : s.by_policy) {
    std::cout << "    policy " << policy << ": " << p.files << " files, " << format_size(p.logical_bytes) << "\n";
  }
  for (const auto& [codec, c] : s.by_codec) {
    std::cout << "    " << codec << ": " << c.records << " records, " << format_size(c.data_bytes) << " stored as "
              << format_size(c.stored_bytes) << " (" << pct(c.stored_bytes, c.data_bytes) << ")\n";
  }
  std::uint64_t used = s.segments_in_use * s.segment_bytes;
  std::cout << "\nSpace:\n"
            << "  segments:          " << s.segments << " of " << format_size(s.segment_bytes) << ": " << s.segments_in_use
            << " in use, " << s.segments_free << " free, " << s.segments_pending << " free after the next checkpoint\n"
            << "  in use:            " << format_size(used) << ": " << format_size(s.live_bytes) << " live, "
            << format_size(used - std::min(used, s.live_bytes))
            << " not live (dead data, the log, and room not yet written in the current segment)\n"
            << "\nGarbage collection since mounting: " << format_size(s.gc_bytes_copied) << " copied in "
            << s.gc_records_copied << " records, " << s.gc_segments_emptied << " segments emptied, "
            << s.gc_packs_repacked << " packs repacked";
  if (s.gc_damaged) std::cout << ", " << s.gc_damaged << " damaged records left in place (run nanoer fsck)";
  std::cout << "\nCold data recompressed since mounting: " << s.tier_chunks << " chunks, "
            << format_size(s.tier_stored_before) << " rewritten as " << format_size(s.tier_stored_after);
  if (s.tier_damaged) std::cout << ", " << s.tier_damaged << " damaged records left in place (run nanoer fsck)";
  std::cout << "\nDedup: " << (s.dedup ? "on" : "off") << ", " << s.dedup_entries << " contents indexed ("
            << format_size(s.dedup_index_bytes) << ")\n"
            << "Metadata in memory: about " << format_size(s.metadata_bytes) << "\n";
}

int cmd_stats(const std::vector<std::string_view>& args) {
  bool codecs = false;
  std::string target;
  for (std::string_view a : args) {
    if (a == "--codecs") {
      codecs = true;
    } else if (!a.empty() && a[0] == '-') {
      return fail(kProgram, "unknown stats option " + std::string(a));
    } else {
      target = std::string(a);
    }
  }
  if (target.empty()) return fail(kProgram, "stats needs a TARGET");
  if (std::filesystem::is_directory(target)) {
    // A mount point: nanoerfs answers this attribute on its root.
    if (codecs) return fail(kProgram, "--codecs needs the filesystem unmounted");
    std::string value(64 << 10, '\0');
    ssize_t n = ::getxattr(target.c_str(), kStatsXattr, value.data(), value.size());
    if (n < 0) return fail(kProgram, target + " is not the mount point of a Nanoer filesystem");
    value.resize(static_cast<std::size_t>(n));
    print_stats(target, decode_stats(value));
    return 0;
  }
  auto store = FileStore::open(target, FileStore::Mode::read_only);
  Filesystem::Options options;
  options.read_only = true;
  options.start_threads = false;
  options.safe_space = 0;
  auto fs = Filesystem::mount(*store, options);
  print_stats(target, fs->space_stats(codecs));
  return 0;
}

// ---- bench -----------------------------------------------------------------

struct Sample {
  Bytes data;
  std::size_t files = 0;
  struct File {
    std::size_t start, size;  // in data
    bool whole;               // all of the file (not cut at the per-file limit)
  };
  std::vector<File> parts;
};

struct Limits {
  std::uint64_t total;
  std::uint64_t per_file;
};

void add_file(Sample& s, const std::filesystem::path& path, const Limits& limits) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::size_t room =
      static_cast<std::size_t>(std::min(limits.total - s.data.size(), limits.per_file));
  std::size_t old = s.data.size();
  s.data.resize(old + room);
  in.read(reinterpret_cast<char*>(s.data.data() + old), static_cast<std::streamsize>(room));
  s.data.resize(old + static_cast<std::size_t>(in.gcount()));
  ++s.files;
  bool whole = in.gcount() < static_cast<std::streamsize>(room) || in.peek() == std::ifstream::traits_type::eof();
  s.parts.push_back({old, s.data.size() - old, whole});
}

Sample collect(const std::vector<std::string>& paths, const Limits& limits) {
  const std::uint64_t max_size = limits.total;
  namespace fs = std::filesystem;
  Sample s;
  for (const auto& p : paths) {
    std::error_code ec;
    if (fs::is_regular_file(fs::symlink_status(p, ec))) {
      add_file(s, p, limits);
    } else if (fs::is_directory(p, ec)) {
      for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (s.data.size() >= max_size) break;
        if (it->is_regular_file(ec) && !it->is_symlink(ec)) add_file(s, it->path(), limits);
      }
    }
    if (s.data.size() >= max_size) break;
  }
  return s;
}

int cmd_bench(const std::vector<std::string_view>& args) {
  std::uint64_t chunk_size = 128 << 10;
  std::uint64_t max_size = 64 << 20;
  std::uint64_t per_file = 4 << 20;
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    std::uint64_t* target = a == "-c" || a == "--chunk-size" ? &chunk_size
                            : a == "-m" || a == "--max-size" ? &max_size
                            : a == "-p" || a == "--per-file" ? &per_file
                                                             : nullptr;
    if (target) {
      auto size = i + 1 < args.size() ? parse_size(args[++i]) : std::nullopt;
      if (!size || *size == 0) return fail(kProgram, std::string(a) + " needs a size such as 128K");
      *target = *size;
    } else {
      paths.emplace_back(a);
    }
  }
  if (paths.empty()) return fail(kProgram, "bench needs at least one PATH");
  if (chunk_size > (1u << kMaxChunkShift)) return fail(kProgram, "chunk size too large");

  Sample sample = collect(paths, {max_size, per_file});
  if (sample.data.empty()) return fail(kProgram, "no readable data found");
  std::cout << "Sample: " << format_size(sample.data.size()) << " from " << sample.files
            << " files, " << format_size(chunk_size) << " chunks, one thread\n\n";
  std::printf("%-6s %6s %9s %14s %16s\n", "codec", "level", "stored", "compress MB/s",
              "decompress MB/s");

  struct Config {
    CodecId codec;
    int level;
  };
  const Config configs[] = {
      {CodecId::lz4, 0},   {CodecId::lz4, 9},   {CodecId::zstd, 1}, {CodecId::zstd, 3},
      {CodecId::zstd, 9},  {CodecId::zstd, 19}, {CodecId::lzma, 1}, {CodecId::lzma, 6},
  };
  using Clock = std::chrono::steady_clock;
  const double mb = static_cast<double>(sample.data.size()) / 1e6;

  for (const Config& c : configs) {
    std::vector<EncodedChunk> chunks;
    auto t0 = Clock::now();
    for (std::size_t off = 0; off < sample.data.size(); off += chunk_size) {
      std::size_t n = std::min<std::size_t>(chunk_size, sample.data.size() - off);
      chunks.push_back(encode_chunk(ByteView(sample.data.data() + off, n), c.codec, c.level));
    }
    auto t1 = Clock::now();
    std::uint64_t stored = 0;
    Bytes out(chunk_size);
    for (const EncodedChunk& chunk : chunks) {
      stored += chunk.payload.size();
      decompress(chunk.codec, chunk.payload, MutableByteView(out.data(), chunk.uncompressed_size));
    }
    auto t2 = Clock::now();
    double compress_s = std::chrono::duration<double>(t1 - t0).count();
    double decompress_s = std::chrono::duration<double>(t2 - t1).count();
    std::printf("%-6s %6d %8.1f%% %14.0f %16.0f\n", std::string(codec_name(c.codec)).c_str(),
                c.level, 100.0 * static_cast<double>(stored) / static_cast<double>(sample.data.size()),
                mb / compress_s, mb / decompress_s);
  }
  std::cout << "\nstored = size on disk as a percentage of the original (incompressible chunks "
               "stored raw)\n";

  // What M4's dedup and small-file packing would save on this sample, with
  // the default codec (zstd level 1).
  std::uint64_t chunks = 0, unique = 0;
  std::set<Blake3Hash> seen;
  std::uint64_t small_files = 0, small_bytes = 0, alone = 0, packed = 0;
  Bytes pack;
  auto stored = [](ByteView d) { return static_cast<std::uint64_t>(encode_chunk(d, CodecId::zstd, 1).payload.size()); };
  for (const Sample::File& f : sample.parts) {
    for (std::size_t off = 0; off < f.size; off += chunk_size) {
      ByteView c(sample.data.data() + f.start + off, std::min<std::size_t>(chunk_size, f.size - off));
      ++chunks;
      unique += seen.insert(blake3(c)).second;
    }
    if (f.whole && f.size > 0 && f.size <= (16u << 10) && f.size <= chunk_size) {
      ByteView d(sample.data.data() + f.start, f.size);
      ++small_files;
      small_bytes += f.size;
      alone += stored(d);
      if (pack.size() + f.size > (64u << 10)) {
        packed += stored(pack);
        pack.clear();
      }
      pack.insert(pack.end(), d.begin(), d.end());
    }
  }
  if (!pack.empty()) packed += stored(pack);
  std::cout << "\nDedup: " << chunks - unique << " of " << chunks << " chunks are duplicates: about "
            << std::fixed << std::setprecision(1)
            << (chunks ? 100.0 * static_cast<double>(chunks - unique) / static_cast<double>(chunks) : 0.0)
            << "% less to store\n";
  if (small_files) {
    std::cout << "Small files: " << small_files << " files up to 16 KiB (" << format_size(small_bytes)
              << "): " << format_size(alone) << " one by one, " << format_size(packed) << " packed ("
              << (100.0 - 100.0 * static_cast<double>(packed) / static_cast<double>(alone)) << "% less)\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  // As fsck.nanoer (for fsck -t nanoer): the arguments are fsck's.
  if (std::string_view(argv[0]).ends_with("fsck.nanoer")) return cmd_fsck(args);
  if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    usage(args.empty() ? std::cerr : std::cout);
    return args.empty() ? 1 : 0;
  }
  std::string_view command = args[0];
  args.erase(args.begin());
  try {
    if (command == "info") return cmd_info(args);
    if (command == "bench") return cmd_bench(args);
    if (command == "fsck") return cmd_fsck(args);
    if (command == "gc") return cmd_gc(args);
    if (command == "stats") return cmd_stats(args);
    if (command == "tier") return cmd_tier(args);
    if (command == "policy") return cmd_policy(args);
    if (command == "version") {
      std::cout << "nanoer " NANOER_VERSION " (format " << kVersionMajor << "." << kVersionMinor << ")\n";
      return 0;
    }
  } catch (const Error& e) {
    return fail(kProgram, e.what());
  }
  usage(std::cerr);
  return fail(kProgram, "unknown command " + std::string(command));
}
