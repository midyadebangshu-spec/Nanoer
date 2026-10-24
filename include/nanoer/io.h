// Backing stores: where a Nanoer filesystem keeps its bytes.
//
//   FileStore   an image file on any filesystem, or a raw block device
//   MemStore    in memory, for tests
//   FaultStore  in memory, simulates power cuts and I/O errors (tests)
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nanoer/bytes.h"

namespace nanoer {

class Store {
 public:
  virtual ~Store() = default;

  virtual std::uint64_t size() const = 0;

  // Reads exactly out.size() bytes or throws.
  virtual void read(std::uint64_t offset, MutableByteView out) = 0;
  // Writes all of `data` or throws. Not durable until flush() returns.
  virtual void write(std::uint64_t offset, ByteView data) = 0;
  // Returns once everything written so far survives a power cut.
  virtual void flush() = 0;

  Bytes read(std::uint64_t offset, std::size_t size);

 protected:
  // Throws Error(invalid_argument) unless [offset, offset + size) fits.
  void check_range(std::uint64_t offset, std::uint64_t size) const;
};

class FileStore final : public Store {
 public:
  enum class Mode { read_only, read_write };

  // Opens an existing image file or block device. Takes an exclusive lock
  // (shared for read_only); throws Error(busy) if another process holds it.
  static std::unique_ptr<FileStore> open(const std::string& path, Mode mode);

  // Creates a new sparse image file of `size` bytes. Fails if it exists.
  static std::unique_ptr<FileStore> create_image(const std::string& path, std::uint64_t size);

  ~FileStore() override;
  FileStore(const FileStore&) = delete;
  FileStore& operator=(const FileStore&) = delete;

  std::uint64_t size() const override { return size_; }
  void read(std::uint64_t offset, MutableByteView out) override;
  void write(std::uint64_t offset, ByteView data) override;
  void flush() override;
  using Store::read;

  bool is_block_device() const { return block_device_; }
  const std::string& path() const { return path_; }

 private:
  FileStore(int fd, std::string path, std::uint64_t size, bool block_device, bool read_only);

  int fd_;
  std::string path_;
  std::uint64_t size_;
  bool block_device_;
  bool read_only_;
};

class MemStore final : public Store {
 public:
  explicit MemStore(std::uint64_t size) : data_(size) {}
  explicit MemStore(Bytes image) : data_(std::move(image)) {}

  std::uint64_t size() const override { return data_.size(); }
  void read(std::uint64_t offset, MutableByteView out) override;
  void write(std::uint64_t offset, ByteView data) override;
  void flush() override {}
  using Store::read;

  Bytes image() const;

 private:
  mutable std::mutex mutex_;
  Bytes data_;
};

// Records which writes have been flushed, so a test can ask what the store
// would contain after a power cut at this moment.
class FaultStore final : public Store {
 public:
  explicit FaultStore(std::uint64_t size);
  explicit FaultStore(Bytes durable_image);

  std::uint64_t size() const override { return current_.size(); }
  void read(std::uint64_t offset, MutableByteView out) override;
  void write(std::uint64_t offset, ByteView data) override;
  void flush() override;
  using Store::read;

  struct CrashOptions {
    // Chance that each unflushed write reached the disk anyway.
    double keep_probability = 0.0;
    // A write that reached the disk may have been cut short, keeping only
    // its first few 512-byte sectors.
    bool torn_writes = false;
    std::uint64_t seed = 0;
  };
  // The contents after a simulated power cut now. Does not change the store.
  Bytes crash_image(const CrashOptions& options) const;

  // Makes the `n`th write or flush from now (counting from 1) and every one
  // after it fail with EIO, as a dying disk would. 0 turns this off.
  void fail_after(std::uint64_t n);

  std::uint64_t write_count() const;
  std::uint64_t flush_count() const;
  std::size_t pending_write_count() const;
  Bytes durable_image() const;

 private:
  void maybe_fail();

  mutable std::mutex mutex_;
  Bytes current_;
  Bytes durable_;
  std::vector<std::pair<std::uint64_t, Bytes>> pending_;
  std::uint64_t writes_ = 0;
  std::uint64_t flushes_ = 0;
  std::uint64_t fail_countdown_ = 0;
  bool failed_ = false;
};

}  // namespace nanoer
