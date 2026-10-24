// mkfs.nanoer: create a Nanoer filesystem on an image file or block device.

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;
using namespace nanoer::tools;

namespace {

constexpr std::string_view kProgram = "mkfs.nanoer";

void usage(std::ostream& out) {
  out << "Usage: mkfs.nanoer [options] TARGET\n"
         "\n"
         "Create a Nanoer filesystem on TARGET, an image file or a block device.\n"
         "\n"
         "Options:\n"
         "  -s, --size SIZE          create TARGET as a new sparse image file of SIZE (e.g. 20G)\n"
         "  -S, --segment-size SIZE  log segment size, a power of two from 1M to 1G (default 64M)\n"
         "  -c, --chunk-size SIZE    compression chunk size, a power of two from 4K to 16M\n"
         "                           (default 128K)\n"
         "      --codec NAME         codec for new data: none, lz4, zstd, lzma (default zstd)\n"
         "  -l, --level N            compression level for new data (default 1)\n"
         "  -L, --label TEXT         filesystem label, up to 63 bytes\n"
         "  -f, --force              overwrite existing data or an existing filesystem\n"
         "  -h, --help               show this help\n";
}

// Refuses to overwrite something that looks in use, unless --force.
std::string existing_contents(Store& store) {
  try {
    LoadedSuperblock loaded = load_superblock(store);
    std::string desc = "a Nanoer filesystem (UUID " + uuid_to_string(loaded.sb.uuid);
    if (!loaded.sb.label.empty()) desc += ", label \"" + loaded.sb.label + "\"";
    return desc + ")";
  } catch (const Error&) {
  }
  // Partition tables and other filesystems keep their signatures near the
  // start; any non-zero byte there means the target is probably in use.
  Bytes head = store.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(store.size(), 64 << 10)));
  if (std::any_of(head.begin(), head.end(), [](std::uint8_t b) { return b != 0; })) {
    return "existing data (possibly a partition table or another filesystem)";
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  FormatOptions options;
  std::optional<std::uint64_t> create_size;
  bool force = false;
  std::string target;

  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= args.size()) return std::nullopt;
      return args[++i];
    };
    auto size_value = [&](std::string_view name) -> std::optional<std::uint64_t> {
      auto v = value();
      auto size = v ? parse_size(*v) : std::nullopt;
      if (!size) fail(kProgram, std::string(name) + " needs a size such as 64M");
      return size;
    };
    auto shift_value = [&](std::string_view name) -> std::optional<unsigned> {
      auto size = size_value(name);
      if (!size) return std::nullopt;
      auto shift = exact_log2(*size);
      if (!shift) fail(kProgram, std::string(name) + " must be a power of two");
      return shift;
    };

    if (a == "-h" || a == "--help") {
      usage(std::cout);
      return 0;
    } else if (a == "-s" || a == "--size") {
      create_size = size_value(a);
      if (!create_size) return 1;
    } else if (a == "-S" || a == "--segment-size") {
      auto shift = shift_value(a);
      if (!shift) return 1;
      options.segment_shift = *shift;
    } else if (a == "-c" || a == "--chunk-size") {
      auto shift = shift_value(a);
      if (!shift) return 1;
      options.chunk_shift = *shift;
    } else if (a == "--codec") {
      auto v = value();
      auto codec = v ? codec_from_name(*v) : std::nullopt;
      if (!codec) return fail(kProgram, "--codec needs one of: none, lz4, zstd, lzma");
      options.codec = *codec;
    } else if (a == "-l" || a == "--level") {
      auto v = value();
      try {
        if (!v) throw std::invalid_argument("missing");
        options.level = std::stoi(std::string(*v));
      } catch (const std::exception&) {
        return fail(kProgram, "--level needs a number");
      }
    } else if (a == "-L" || a == "--label") {
      auto v = value();
      if (!v) return fail(kProgram, "--label needs a value");
      options.label = std::string(*v);
    } else if (a == "-f" || a == "--force") {
      force = true;
    } else if (!a.empty() && a[0] == '-') {
      usage(std::cerr);
      return fail(kProgram, "unknown option " + std::string(a));
    } else if (target.empty()) {
      target = std::string(a);
    } else {
      return fail(kProgram, "only one TARGET may be given");
    }
  }
  if (target.empty()) {
    usage(std::cerr);
    return 1;
  }
  if (options.codec == CodecId::none) options.level = 0;

  bool created = false;
  try {
    std::unique_ptr<FileStore> store;
    if (create_size) {
      store = FileStore::create_image(target, *create_size);
      created = true;
    } else {
      store = FileStore::open(target, FileStore::Mode::read_write);
      if (std::string existing = existing_contents(*store); !existing.empty() && !force) {
        return fail(kProgram, target + " contains " + existing + ". Use --force to overwrite it.");
      }
    }

    Superblock sb = make_filesystem(*store, options);
    std::cout << "Created a Nanoer filesystem on " << target
              << (store->is_block_device() ? " (block device)" : " (image file)") << "\n"
              << "  UUID:          " << uuid_to_string(sb.uuid) << "\n";
    if (!sb.label.empty()) std::cout << "  label:         " << sb.label << "\n";
    std::cout << "  size:          " << format_size(sb.store_size) << "\n"
              << "  segments:      " << sb.segment_count << " x " << format_size(sb.segment_size())
              << "\n"
              << "  chunk size:    " << format_size(sb.chunk_size()) << "\n"
              << "  new data:      " << codec_name(sb.default_codec) << " level "
              << int{sb.default_level} << "\n";
    return 0;
  } catch (const Error& e) {
    // Do not leave a half-made image behind (but never delete an existing file).
    if (created) std::remove(target.c_str());
    return fail(kProgram, e.what());
  }
}
