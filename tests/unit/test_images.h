// Tests: the filesystem images made by older builds (tests/images, made by
// make_images.sh), decompressed.
#pragma once

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <lzma.h>

#include "nanoer/bytes.h"

namespace nanoer {

// The images there: made by the M2, M3, M4 and M5 builds.
inline const std::vector<std::string> kOldImages = {"m2", "m3", "m4", "m5"};

inline Bytes load_test_image(const std::string& name) {
  std::string path = std::string(NANOER_TEST_IMAGES_DIR) + "/" + name + ".img.xz";
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::vector<std::uint8_t> packed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Bytes out(64 << 20);  // every image is 64 MiB
  std::uint64_t memlimit = UINT64_MAX;
  std::size_t in_pos = 0, out_pos = 0;
  lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, nullptr, packed.data(), &in_pos, packed.size(), out.data(),
                                         &out_pos, out.size());
  if (r != LZMA_OK || out_pos != out.size()) throw std::runtime_error("cannot decompress " + path);
  return out;
}

}  // namespace nanoer
