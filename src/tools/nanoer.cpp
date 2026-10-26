// nanoer: inspect Nanoer filesystems and measure codecs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
         "  version                   print the version\n"
         "\n"
         "fsck exit codes: 0 clean, 1 errors fixed, 4 errors left, 8 could not run.\n"
         "Also installed as fsck.nanoer, for fsck -t nanoer and boot-time checks.\n";
}

std::string features(std::uint64_t f, std::uint64_t named = 0, const char* name = nullptr) {
  if (f == 0) return "none";
  std::string out;
  if (f & named) {
    out = name;
    f &= ~named;
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
            << "  features:        compat " << features(sb.features_compat) << ", ro_compat "
            << features(sb.features_ro_compat) << ", incompat " << features(sb.features_incompat, incompat_features::shared_records, "shared-records")
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

// ---- bench -----------------------------------------------------------------

struct Sample {
  Bytes data;
  std::size_t files = 0;
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
