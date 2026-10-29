// nanoer-scale: Nanoer at scale (docs/M6_PLAN.md §1, done-when 1 and 2).
//
//   nanoer-scale make IMAGE FILES CHUNKS_PER_FILE [SIZE]
//       a new image of SIZE (default 64G, sparse) holding FILES files of
//       CHUNKS_PER_FILE full 128 KiB chunks, 5,000 files per directory;
//       every chunk different (dedup finds nothing) but compressible, so a
//       filesystem of ~500 GB of data fits in a few GB
//   nanoer-scale measure IMAGE
//       mounts it in-process and reports: mount time and memory; the time
//       to rebuild the dedup index; memory after reading every file's
//       attributes and first chunk; what a checkpoint writes after one
//       small change and after 10,000 (bytes reaching the store, time); and
//       mount after a crash with a log to replay.
//
// Memory is the process's resident set (VmRSS / VmHWM) from /proc.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

#include <sys/stat.h>

#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

// Counts what reaches the store.
class CountingStore final : public Store {
 public:
  explicit CountingStore(Store& inner) : inner_(inner) {}
  std::uint64_t size() const override { return inner_.size(); }
  void read(std::uint64_t offset, MutableByteView out) override { inner_.read(offset, out); }
  void write(std::uint64_t offset, ByteView data) override {
    written += data.size();
    inner_.write(offset, data);
  }
  void flush() override {
    ++flushes;
    inner_.flush();
  }
  void discard(std::uint64_t offset, std::uint64_t length) override { inner_.discard(offset, length); }
  using Store::read;
  std::uint64_t written = 0;
  std::uint64_t flushes = 0;

 private:
  Store& inner_;
};

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// VmRSS and VmHWM, in MiB.
std::pair<double, double> memory() {
  std::ifstream in("/proc/self/status");
  std::string line;
  double rss = 0, hwm = 0;
  while (std::getline(in, line)) {
    if (line.rfind("VmRSS:", 0) == 0) rss = std::stod(line.substr(6)) / 1024;
    if (line.rfind("VmHWM:", 0) == 0) hwm = std::stod(line.substr(6)) / 1024;
  }
  return {rss, hwm};
}

void report(const char* what, double seconds) {
  auto [rss, hwm] = memory();
  std::printf("%-46s %9.2f s   RSS %7.1f MiB   peak %7.1f MiB\n", what, seconds, rss, hwm);
  std::fflush(stdout);
}

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.start_threads = false;
  o.cold_after_s = -1;  // no background recompression while measuring
  return o;
}

std::string dir_name(long i) { return "d" + std::to_string(i / 5000); }

int make(const std::string& path, long files, long chunks, std::uint64_t size) {
  auto store = FileStore::create_image(path, size);
  make_filesystem(*store, {});
  auto fs = Filesystem::mount(*store, options());
  fs->start();
  const std::size_t chunk = fs->superblock().chunk_size();
  Bytes data(chunk * static_cast<std::size_t>(chunks));
  // A short text repeated: compresses to a few hundred bytes a chunk.
  const char* pattern = "nanoer scale test: segment chunk record tree node checkpoint\n";
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(pattern[i % 61]);
  InodeId dir = 0;
  double start = now_s();
  for (long f = 0; f < files; ++f) {
    if (f % 5000 == 0) dir = fs->mkdir(kRootInode, dir_name(f), 0755, {}).ino;
    // Every chunk different: its first bytes name it.
    for (long c = 0; c < chunks; ++c) {
      char tag[64];
      int n = std::snprintf(tag, sizeof tag, "file %ld chunk %ld\n", f, c);
      std::memcpy(data.data() + static_cast<std::size_t>(c) * chunk, tag, static_cast<std::size_t>(n));
    }
    InodeId ino = fs->mknod(dir, "f" + std::to_string(f), S_IFREG | 0644, 0, {}).ino;
    fs->write(ino, 0, data);
    if (f % 50000 == 49999) {
      fs->sync();
      std::fprintf(stderr, "%ld files, %.0f s\n", f + 1, now_s() - start);
    }
  }
  fs->unmount();
  FsInfo info = fs->info();
  std::printf("%s: %ld files, %ld chunks, %.1f GiB logical, %.0f s\n", path.c_str(), files, files * chunks,
              static_cast<double>(files * chunks) * static_cast<double>(chunk) / (1 << 30), now_s() - start);
  (void)info;
  return 0;
}

int measure(const std::string& path) {
  auto file = FileStore::open(path, FileStore::Mode::read_write);
  CountingStore store(*file);
  report("start", 0);

  // Mount (the image was unmounted cleanly: nothing to replay).
  double t = now_s();
  auto fs = Filesystem::mount(store, options());
  report("mount", now_s() - t);
  FsInfo info = fs->info();
  std::printf("  %llu files, metadata tree %.1f MiB on disk\n", static_cast<unsigned long long>(info.files),
              static_cast<double>(fs->space_stats(false).metadata_disk_bytes) / (1 << 20));

  t = now_s();
  fs->rebuild_dedup();
  info = fs->info();
  report("dedup index rebuilt", now_s() - t);
  std::printf("  %llu contents indexed in %.1f MiB\n", static_cast<unsigned long long>(info.dedup_entries),
              static_cast<double>(info.dedup_index_bytes) / (1 << 20));

  // Every file's attributes, and the first chunk of 1 in 100 files.
  t = now_s();
  std::uint64_t seen = 0, read = 0;
  Bytes buf(4096);
  for (const DirEntry& d : fs->list(kRootInode)) {
    if (d.name == "." || d.name == "..") continue;
    for (const DirEntry& e : fs->list(d.ino)) {
      if (e.name == "." || e.name == "..") continue;
      fs->getattr(e.ino);
      if (++seen % 100 == 0) read += fs->read(e.ino, 0, buf);
    }
  }
  report("stat every file, read 1 in 100", now_s() - t);
  std::printf("  %llu files, %llu bytes read\n", static_cast<unsigned long long>(seen),
              static_cast<unsigned long long>(read));

  // A checkpoint after one small change.
  auto change = [&](long n) {
    for (long i = 0; i < n; ++i) {
      long f = (i * 7919) % static_cast<long>(seen);
      InodeId dir = fs->lookup(kRootInode, dir_name(f)).ino;
      InodeId ino = fs->lookup(dir, "f" + std::to_string(f)).ino;
      std::string s = "changed " + std::to_string(i);
      fs->write(ino, 4096, ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    }
  };
  for (long n : {1L, 10000L}) {
    change(n);
    fs->sync();
    std::uint64_t before = store.written;
    t = now_s();
    fs->checkpoint_now();
    double s = now_s() - t;
    char what[64];
    std::snprintf(what, sizeof what, "checkpoint after %ld change%s", n, n == 1 ? "" : "s");
    report(what, s);
    std::printf("  %.2f MiB written (tree nodes and the checkpoint record)\n",
                static_cast<double>(store.written - before) / (1 << 20));
  }

  // A crash with work in the log: 2,000 more changes, committed, then gone.
  change(2000);
  fs->sync();
  fs->abandon();
  fs.reset();
  t = now_s();
  fs = Filesystem::mount(store, options());
  report("mount after a crash (2,000 changes to replay)", now_s() - t);
  fs->unmount();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  try {
    if (cmd == "make" && argc >= 5) {
      return make(argv[2], std::stol(argv[3]), std::stol(argv[4]),
                  argc > 5 ? std::stoull(argv[5]) << 30 : std::uint64_t{64} << 30);
    }
    if (cmd == "measure" && argc == 3) return measure(argv[2]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "nanoer-scale: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "usage: nanoer-scale make IMAGE FILES CHUNKS_PER_FILE [SIZE_GIB] | measure IMAGE\n");
  return 2;
}
