// nanoer-fsx: a file system exerciser in the spirit of the classic fsx.
//
// Performs random reads, writes, truncates, hole punches and memory-mapped
// reads and writes on one file, and checks every byte read against a copy
// kept in memory. Any mismatch is reported with the operations that led to
// it. Works on any filesystem; used to test Nanoer through a real mount.

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Settings {
  std::uint64_t ops = 10000;
  std::uint64_t seed = 1;
  std::uint64_t max_size = 4 << 20;  // the file never grows beyond this
  std::uint64_t max_op = 256 << 10;  // largest single read or write
  bool mmap = true;
  bool punch = true;
  bool keep = false;  // leave the file behind (for checking after a remount)
  std::string path;
};

class Exerciser {
 public:
  explicit Exerciser(const Settings& s) : s_(s), rng_(s.seed) {}

  int run() {
    fd_ = ::open(s_.path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) return fail("open", errno);
    for (std::uint64_t i = 0; i < s_.ops; ++i) {
      op_number_ = i + 1;
      int r = step();
      if (r) return r;
      if (i % 1000 == 999 && verify_all()) return 1;
    }
    if (verify_all()) return 1;
    if (!s_.keep) ::unlink(s_.path.c_str());
    ::close(fd_);
    std::printf("nanoer-fsx: %llu operations, final size %zu, no mismatches\n",
                static_cast<unsigned long long>(s_.ops), good_.size());
    return 0;
  }

 private:
  std::uint64_t random(std::uint64_t n) { return n ? rng_() % n : 0; }

  void note(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    history_.push_back(std::to_string(op_number_) + ": " + buf);
    if (history_.size() > 30) history_.pop_front();
  }

  int fail(const char* what, int err) {
    std::fprintf(stderr, "nanoer-fsx: %s failed at operation %llu: %s\n", what,
                 static_cast<unsigned long long>(op_number_), std::strerror(err));
    dump_history();
    return 1;
  }

  void dump_history() {
    std::fprintf(stderr, "last operations:\n");
    for (const auto& h : history_) std::fprintf(stderr, "  %s\n", h.c_str());
  }

  int compare(const std::vector<std::uint8_t>& got, std::uint64_t off, const char* how) {
    for (std::size_t i = 0; i < got.size(); ++i) {
      std::uint8_t want = off + i < good_.size() ? good_[off + i] : 0;
      if (got[i] != want) {
        std::fprintf(stderr,
                     "nanoer-fsx: MISMATCH (%s) at byte %llu: expected 0x%02x, got 0x%02x "
                     "(operation %llu, seed %llu)\n",
                     how, static_cast<unsigned long long>(off + i), want, got[i],
                     static_cast<unsigned long long>(op_number_),
                     static_cast<unsigned long long>(s_.seed));
        dump_history();
        return 1;
      }
    }
    return 0;
  }

  int step() {
    std::uint64_t pick = random(100);
    if (pick < 30) return do_read();
    if (pick < 60) return do_write();
    if (pick < 68) return do_truncate();
    if (pick < 74 && s_.punch) return do_punch();
    if (pick < 84 && s_.mmap) return do_map_read();
    if (pick < 94 && s_.mmap) return do_map_write();
    if (pick < 97) {
      note("fsync");
      return ::fsync(fd_) ? fail("fsync", errno) : 0;
    }
    note("close and reopen");
    ::close(fd_);
    fd_ = ::open(s_.path.c_str(), O_RDWR);
    return fd_ < 0 ? fail("reopen", errno) : 0;
  }

  int do_read() {
    if (good_.empty()) return 0;
    std::uint64_t off = random(good_.size());
    std::uint64_t len = 1 + random(std::min<std::uint64_t>(s_.max_op, good_.size() - off));
    note("read %llu+%llu", static_cast<unsigned long long>(off), static_cast<unsigned long long>(len));
    std::vector<std::uint8_t> got(len);
    ssize_t n = ::pread(fd_, got.data(), len, static_cast<off_t>(off));
    if (n < 0) return fail("read", errno);
    if (static_cast<std::uint64_t>(n) != len) {
      std::fprintf(stderr, "nanoer-fsx: short read: %zd of %llu\n", n, static_cast<unsigned long long>(len));
      dump_history();
      return 1;
    }
    return compare(got, off, "read");
  }

  std::vector<std::uint8_t> payload(std::uint64_t len) {
    std::vector<std::uint8_t> data(len);
    std::uint8_t base = static_cast<std::uint8_t>(op_number_);
    bool noisy = random(2);
    for (std::uint64_t i = 0; i < len; ++i) {
      data[i] = noisy ? static_cast<std::uint8_t>(rng_()) : static_cast<std::uint8_t>(base + i % 7);
    }
    return data;
  }

  int do_write() {
    std::uint64_t off = random(s_.max_size);
    std::uint64_t len = 1 + random(std::min(s_.max_op, s_.max_size - off));
    note("write %llu+%llu", static_cast<unsigned long long>(off), static_cast<unsigned long long>(len));
    std::vector<std::uint8_t> data = payload(len);
    ssize_t n = ::pwrite(fd_, data.data(), len, static_cast<off_t>(off));
    if (n < 0) return fail("write", errno);
    if (static_cast<std::uint64_t>(n) != len) return fail("short write", EIO);
    if (good_.size() < off + len) good_.resize(off + len, 0);
    std::copy(data.begin(), data.end(), good_.begin() + static_cast<std::ptrdiff_t>(off));
    return 0;
  }

  int do_truncate() {
    std::uint64_t size = random(s_.max_size);
    note("truncate %llu", static_cast<unsigned long long>(size));
    if (::ftruncate(fd_, static_cast<off_t>(size))) return fail("truncate", errno);
    good_.resize(size, 0);
    return 0;
  }

  int do_punch() {
    if (good_.empty()) return 0;
    std::uint64_t off = random(good_.size());
    std::uint64_t len = 1 + random(std::min<std::uint64_t>(s_.max_op, good_.size() - off));
    note("punch %llu+%llu", static_cast<unsigned long long>(off), static_cast<unsigned long long>(len));
    if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(off),
                    static_cast<off_t>(len))) {
      if (errno == EOPNOTSUPP) {
        s_.punch = false;
        return 0;
      }
      return fail("punch hole", errno);
    }
    std::fill(good_.begin() + static_cast<std::ptrdiff_t>(off),
              good_.begin() + static_cast<std::ptrdiff_t>(off + len), 0);
    return 0;
  }

  int do_map_read() {
    if (good_.empty()) return 0;
    std::uint64_t off = random(good_.size());
    std::uint64_t len = 1 + random(std::min<std::uint64_t>(s_.max_op, good_.size() - off));
    note("mapread %llu+%llu", static_cast<unsigned long long>(off), static_cast<unsigned long long>(len));
    std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    std::uint64_t start = off / page * page;
    void* p = ::mmap(nullptr, off + len - start, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(start));
    if (p == MAP_FAILED) return fail("mmap for reading", errno);
    std::vector<std::uint8_t> got(len);
    std::memcpy(got.data(), static_cast<std::uint8_t*>(p) + (off - start), len);
    ::munmap(p, off + len - start);
    return compare(got, off, "mmap read");
  }

  int do_map_write() {
    std::uint64_t off = random(s_.max_size);
    std::uint64_t len = 1 + random(std::min(s_.max_op, s_.max_size - off));
    note("mapwrite %llu+%llu", static_cast<unsigned long long>(off), static_cast<unsigned long long>(len));
    // A mapping cannot extend the file: grow it first, as fsx does.
    if (good_.size() < off + len) {
      if (::ftruncate(fd_, static_cast<off_t>(off + len))) return fail("truncate before mapwrite", errno);
      good_.resize(off + len, 0);
    }
    std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    std::uint64_t start = off / page * page;
    void* p = ::mmap(nullptr, off + len - start, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                     static_cast<off_t>(start));
    if (p == MAP_FAILED) return fail("mmap for writing", errno);
    std::vector<std::uint8_t> data = payload(len);
    std::memcpy(static_cast<std::uint8_t*>(p) + (off - start), data.data(), len);
    if (::msync(p, off + len - start, MS_SYNC)) {
      int e = errno;
      ::munmap(p, off + len - start);
      return fail("msync", e);
    }
    ::munmap(p, off + len - start);
    std::copy(data.begin(), data.end(), good_.begin() + static_cast<std::ptrdiff_t>(off));
    return 0;
  }

  int verify_all() {
    note("verify whole file");
    struct stat st;
    if (::fstat(fd_, &st)) return fail("fstat", errno);
    if (static_cast<std::uint64_t>(st.st_size) != good_.size()) {
      std::fprintf(stderr, "nanoer-fsx: size is %lld, expected %zu\n", static_cast<long long>(st.st_size),
                   good_.size());
      dump_history();
      return 1;
    }
    std::vector<std::uint8_t> got(good_.size());
    std::size_t done = 0;
    while (done < got.size()) {
      ssize_t n = ::pread(fd_, got.data() + done, got.size() - done, static_cast<off_t>(done));
      if (n <= 0) return fail("read for verify", n < 0 ? errno : EIO);
      done += static_cast<std::size_t>(n);
    }
    return compare(got, 0, "full verify");
  }

  Settings s_;
  std::mt19937_64 rng_;
  int fd_ = -1;
  std::uint64_t op_number_ = 0;
  std::vector<std::uint8_t> good_;
  std::deque<std::string> history_;
};

void usage() {
  std::fprintf(stderr,
               "Usage: nanoer-fsx [options] FILE\n"
               "  -N COUNT   operations (default 10000)\n"
               "  -S SEED    random seed (default 1)\n"
               "  -l SIZE    largest file size in bytes (default 4194304)\n"
               "  -o SIZE    largest single read or write (default 262144)\n"
               "  -M         no mmap operations\n"
               "  -P         no hole punching\n"
               "  -k         keep the file afterwards\n");
}

}  // namespace

int main(int argc, char** argv) {
  Settings s;
  int opt;
  while ((opt = ::getopt(argc, argv, "N:S:l:o:MPkh")) != -1) {
    switch (opt) {
      case 'N': s.ops = std::strtoull(optarg, nullptr, 10); break;
      case 'S': s.seed = std::strtoull(optarg, nullptr, 10); break;
      case 'l': s.max_size = std::max<std::uint64_t>(1, std::strtoull(optarg, nullptr, 10)); break;
      case 'o': s.max_op = std::max<std::uint64_t>(1, std::strtoull(optarg, nullptr, 10)); break;
      case 'M': s.mmap = false; break;
      case 'P': s.punch = false; break;
      case 'k': s.keep = true; break;
      default:
        usage();
        return opt == 'h' ? 0 : 2;
    }
  }
  if (optind + 1 != argc) {
    usage();
    return 2;
  }
  s.path = argv[optind];
  return Exerciser(s).run();
}
