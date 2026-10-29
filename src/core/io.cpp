#include "nanoer/io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nanoer/error.h"

namespace nanoer {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  int e = errno;
  throw Error(Errc::io, what + ": " + std::strerror(e), e);
}

}  // namespace

Bytes Store::read(std::uint64_t offset, std::size_t size) {
  Bytes out(size);
  read(offset, out);
  return out;
}

void Store::check_range(std::uint64_t offset, std::uint64_t size) const {
  if (offset > this->size() || size > this->size() - offset) {
    throw Error(Errc::invalid_argument, "access beyond the end of the store (offset " +
                                            std::to_string(offset) + ", " + std::to_string(size) +
                                            " bytes)");
  }
}

// ---- FileStore -------------------------------------------------------------

FileStore::FileStore(int fd, std::string path, std::uint64_t size, bool block_device, bool read_only)
    : fd_(fd), path_(std::move(path)), size_(size), block_device_(block_device), read_only_(read_only) {}

FileStore::~FileStore() { ::close(fd_); }

std::unique_ptr<FileStore> FileStore::open(const std::string& path, Mode mode) {
  bool read_only = mode == Mode::read_only;
  int fd = ::open(path.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (fd < 0) throw_errno("cannot open " + path);

  // Two processes writing one store would destroy it.
  if (::flock(fd, (read_only ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
    int e = errno;
    ::close(fd);
    if (e == EWOULDBLOCK) throw Error(Errc::busy, path + " is in use by another process", e);
    errno = e;
    throw_errno("cannot lock " + path);
  }

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    int e = errno;
    ::close(fd);
    errno = e;
    throw_errno("cannot stat " + path);
  }
  std::uint64_t size = 0;
  bool block_device = S_ISBLK(st.st_mode);
  if (block_device) {
    if (::ioctl(fd, BLKGETSIZE64, &size) != 0) {
      int e = errno;
      ::close(fd);
      errno = e;
      throw_errno("cannot get the size of " + path);
    }
  } else if (S_ISREG(st.st_mode)) {
    size = static_cast<std::uint64_t>(st.st_size);
  } else {
    ::close(fd);
    throw Error(Errc::invalid_argument, path + " is not a regular file or block device");
  }
  return std::unique_ptr<FileStore>(new FileStore(fd, path, size, block_device, read_only));
}

std::unique_ptr<FileStore> FileStore::create_image(const std::string& path, std::uint64_t size) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw_errno("cannot create " + path);
  // ftruncate makes a sparse file: no disk space is used until data is written.
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    int e = errno;
    ::close(fd);
    ::unlink(path.c_str());
    errno = e;
    throw_errno("cannot size " + path);
  }
  ::close(fd);
  return open(path, Mode::read_write);
}

void FileStore::read(std::uint64_t offset, MutableByteView out) {
  check_range(offset, out.size());
  std::size_t done = 0;
  while (done < out.size()) {
    ssize_t n = ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("read from " + path_ + " failed");
    }
    if (n == 0) throw Error(Errc::io, "unexpected end of " + path_, EIO);
    done += static_cast<std::size_t>(n);
  }
}

void FileStore::write(std::uint64_t offset, ByteView data) {
  if (read_only_) throw Error(Errc::invalid_argument, path_ + " is open read-only");
  check_range(offset, data.size());
  std::size_t done = 0;
  while (done < data.size()) {
    ssize_t n =
        ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("write to " + path_ + " failed");
    }
    done += static_cast<std::size_t>(n);
  }
}

void FileStore::flush() {
  if (read_only_) return;
  // fdatasync also flushes the disk's own write cache, which is what makes
  // the data survive a power cut, not just a crash of this process.
  while (::fdatasync(fd_) != 0) {
    if (errno != EINTR) throw_errno("flush of " + path_ + " failed");
  }
}

void FileStore::discard(std::uint64_t offset, std::uint64_t length) {
  if (read_only_ || length == 0) return;
  check_range(offset, length);
  if (block_device_) {
    std::uint64_t range[2] = {offset, length};
    (void)::ioctl(fd_, BLKDISCARD, &range);  // (unsupported: nothing to reclaim)
  } else {
    (void)::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                      static_cast<off_t>(length));
  }
}

// ---- MemStore --------------------------------------------------------------

void MemStore::read(std::uint64_t offset, MutableByteView out) {
  std::lock_guard lock(mutex_);
  check_range(offset, out.size());
  std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
}

void MemStore::write(std::uint64_t offset, ByteView data) {
  std::lock_guard lock(mutex_);
  check_range(offset, data.size());
  std::copy(data.begin(), data.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void MemStore::discard(std::uint64_t offset, std::uint64_t length) {
  std::lock_guard lock(mutex_);
  check_range(offset, length);
  std::fill_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), length, 0);
}

Bytes MemStore::image() const {
  std::lock_guard lock(mutex_);
  return data_;
}

// ---- FaultStore ------------------------------------------------------------

FaultStore::FaultStore(std::uint64_t size) : current_(size), durable_(size) {}

FaultStore::FaultStore(Bytes durable_image) : current_(durable_image), durable_(std::move(durable_image)) {}

void FaultStore::maybe_fail() {
  if (failed_) throw Error(Errc::io, "simulated disk failure", EIO);
  if (fail_countdown_ != 0 && --fail_countdown_ == 0) {
    failed_ = true;
    throw Error(Errc::io, "simulated disk failure", EIO);
  }
}

void FaultStore::read(std::uint64_t offset, MutableByteView out) {
  std::lock_guard lock(mutex_);
  check_range(offset, out.size());
  std::copy_n(current_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
}

void FaultStore::write(std::uint64_t offset, ByteView data) {
  std::lock_guard lock(mutex_);
  check_range(offset, data.size());
  maybe_fail();
  ++writes_;
  std::copy(data.begin(), data.end(), current_.begin() + static_cast<std::ptrdiff_t>(offset));
  pending_.emplace_back(offset, Bytes(data.begin(), data.end()));
}

void FaultStore::discard(std::uint64_t offset, std::uint64_t length) {
  std::lock_guard lock(mutex_);
  check_range(offset, length);
  std::fill_n(current_.begin() + static_cast<std::ptrdiff_t>(offset), length, 0);
  pending_.emplace_back(offset, Bytes(length, 0));
}

void FaultStore::flush() {
  std::lock_guard lock(mutex_);
  maybe_fail();
  ++flushes_;
  durable_ = current_;
  pending_.clear();
}

Bytes FaultStore::crash_image(const CrashOptions& options) const {
  constexpr std::size_t kSector = 512;
  std::lock_guard lock(mutex_);
  Bytes image = durable_;
  std::mt19937_64 rng(options.seed);
  std::bernoulli_distribution keep(options.keep_probability);
  // Disks may persist unflushed writes in any order, so each one is decided
  // independently. Applying the kept ones in issue order means a later write
  // to the same place wins, as on a real disk.
  for (const auto& [offset, data] : pending_) {
    if (!keep(rng)) continue;
    std::size_t length = data.size();
    if (options.torn_writes && length > kSector) {
      std::size_t sectors = (length + kSector - 1) / kSector;
      length = std::min(length, std::uniform_int_distribution<std::size_t>(1, sectors)(rng) * kSector);
    }
    std::copy_n(data.begin(), length, image.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return image;
}

void FaultStore::fail_after(std::uint64_t n) {
  std::lock_guard lock(mutex_);
  fail_countdown_ = n;
  failed_ = false;
}

std::uint64_t FaultStore::write_count() const {
  std::lock_guard lock(mutex_);
  return writes_;
}

std::uint64_t FaultStore::flush_count() const {
  std::lock_guard lock(mutex_);
  return flushes_;
}

std::size_t FaultStore::pending_write_count() const {
  std::lock_guard lock(mutex_);
  return pending_.size();
}

Bytes FaultStore::durable_image() const {
  std::lock_guard lock(mutex_);
  return durable_;
}

}  // namespace nanoer
