#include "nanoer/io.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "nanoer/error.h"

using namespace nanoer;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "nanoer-test-XXXXXX").string();
    path_ = ::mkdtemp(tmpl.data());
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

Bytes pattern(std::size_t n, std::uint8_t start) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(start + i);
  return b;
}

}  // namespace

TEST(MemStore, ReadWriteAndBounds) {
  MemStore s(1000);
  s.write(10, pattern(20, 5));
  EXPECT_EQ(s.read(10, 20), pattern(20, 5));
  EXPECT_EQ(s.read(0, 10), Bytes(10, 0));
  EXPECT_THROW(s.read(990, 11), Error);
  EXPECT_THROW(s.write(1000, pattern(1, 0)), Error);
  EXPECT_NO_THROW(s.read(1000, 0));
}

TEST(FileStore, CreateWriteReopenRead) {
  TempDir dir;
  std::string path = dir.file("store.img");
  {
    auto s = FileStore::create_image(path, 1 << 20);
    EXPECT_EQ(s->size(), 1u << 20);
    EXPECT_FALSE(s->is_block_device());
    s->write(4096, pattern(100, 1));
    s->flush();
    EXPECT_THROW(s->read((1 << 20) - 10, 11), Error);
  }
  auto s = FileStore::open(path, FileStore::Mode::read_only);
  EXPECT_EQ(s->read(4096, 100), pattern(100, 1));
  EXPECT_THROW(s->write(0, pattern(1, 0)), Error);
}

TEST(FileStore, CreateRefusesExistingFile) {
  TempDir dir;
  std::string path = dir.file("store.img");
  FileStore::create_image(path, 4096);
  EXPECT_THROW(FileStore::create_image(path, 4096), Error);
}

TEST(FileStore, ExclusiveLockPreventsSecondWriter) {
  TempDir dir;
  std::string path = dir.file("store.img");
  auto first = FileStore::create_image(path, 4096);
  try {
    FileStore::open(path, FileStore::Mode::read_write);
    FAIL() << "a second writer was allowed";
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::busy);
  }
  EXPECT_THROW(FileStore::open(path, FileStore::Mode::read_only), Error);
  first.reset();
  EXPECT_NO_THROW(FileStore::open(path, FileStore::Mode::read_write));
}

TEST(FileStore, MissingFileIsIoError) {
  try {
    FileStore::open("/nonexistent/nanoer.img", FileStore::Mode::read_only);
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::io);
    EXPECT_EQ(e.to_errno(), ENOENT);
  }
}

TEST(FaultStore, CrashDropsUnflushedWrites) {
  FaultStore s(8192);
  s.write(0, pattern(100, 1));
  s.flush();
  s.write(100, pattern(100, 2));
  EXPECT_EQ(s.pending_write_count(), 1u);
  EXPECT_EQ(s.read(100, 100), pattern(100, 2));  // visible before the crash

  Bytes lost = s.crash_image({});
  EXPECT_EQ(Bytes(lost.begin(), lost.begin() + 100), pattern(100, 1));
  EXPECT_EQ(Bytes(lost.begin() + 100, lost.begin() + 200), Bytes(100, 0));

  Bytes kept = s.crash_image({.keep_probability = 1.0});
  EXPECT_EQ(Bytes(kept.begin() + 100, kept.begin() + 200), pattern(100, 2));
}

TEST(FaultStore, TornWritesKeepWholeSectorsOnly) {
  FaultStore s(8192);
  s.write(0, Bytes(4096, 0xab));
  bool saw_partial = false;
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    Bytes img = s.crash_image({.keep_probability = 1.0, .torn_writes = true, .seed = seed});
    std::size_t written = 0;
    while (written < 4096 && img[written] == 0xab) ++written;
    EXPECT_EQ(written % 512, 0u);
    EXPECT_GE(written, 512u);
    for (std::size_t i = written; i < 4096; ++i) ASSERT_EQ(img[i], 0);
    saw_partial |= written < 4096;
  }
  EXPECT_TRUE(saw_partial);
}

TEST(FaultStore, LaterWriteToSameRangeWins) {
  FaultStore s(4096);
  s.write(0, Bytes(10, 1));
  s.write(0, Bytes(10, 2));
  Bytes img = s.crash_image({.keep_probability = 1.0});
  EXPECT_EQ(img[0], 2);
}

TEST(FaultStore, InjectedFailureIsPermanent) {
  FaultStore s(4096);
  s.fail_after(2);
  EXPECT_NO_THROW(s.write(0, Bytes(1, 1)));
  EXPECT_THROW(s.flush(), Error);
  EXPECT_THROW(s.write(0, Bytes(1, 1)), Error);
  EXPECT_EQ(s.flush_count(), 0u);
  s.fail_after(0);
  EXPECT_NO_THROW(s.flush());
}
