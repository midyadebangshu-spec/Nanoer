// nanoerfs: mount a Nanoer filesystem through FUSE.
//
// A thin adapter: each FUSE request becomes one Filesystem call, and
// nanoer::Error becomes an errno. Also installed as mount.nanoer, so that
// `mount -t nanoer IMAGE DIR` and /etc/fstab work.

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"

using namespace nanoer;

namespace {

// How long the kernel may cache attributes and names, in seconds. Every
// change goes through this mount (the store is locked against any other
// writer), and the kernel updates its own caches for those changes, so the
// caches cannot go stale: a long timeout only saves round trips.
constexpr double kTimeout = 60.0;

struct Context {
  std::unique_ptr<FileStore> store;
  std::unique_ptr<Filesystem> fs;
};

Filesystem& fs_of(fuse_req_t req) { return *static_cast<Context*>(fuse_req_userdata(req))->fs; }

// Runs `body`, which must send the reply as its last action. Any error
// becomes an error reply instead.
template <class F>
void guarded(fuse_req_t req, F&& body) {
  try {
    body();
  } catch (const Error& e) {
    if (e.code() != Errc::posix) std::fprintf(stderr, "nanoerfs: %s\n", e.what());
    fuse_reply_err(req, e.to_errno());
  } catch (const std::bad_alloc&) {
    fuse_reply_err(req, ENOMEM);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "nanoerfs: %s\n", e.what());
    fuse_reply_err(req, EIO);
  }
}

timespec to_timespec(std::int64_t ns) {
  std::int64_t sec = ns / 1'000'000'000;
  std::int64_t rem = ns % 1'000'000'000;
  if (rem < 0) {  // round towards minus infinity, as timespec requires
    rem += 1'000'000'000;
    --sec;
  }
  return {static_cast<time_t>(sec), static_cast<long>(rem)};
}

std::int64_t from_timespec(const timespec& t) {
  return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

struct stat to_stat(const Attr& a) {
  struct stat st {};
  st.st_ino = a.ino;
  st.st_mode = a.mode;
  st.st_nlink = a.nlink;
  st.st_uid = a.uid;
  st.st_gid = a.gid;
  st.st_size = static_cast<off_t>(a.size);
  st.st_blocks = static_cast<blkcnt_t>(a.blocks);
  st.st_blksize = static_cast<blksize_t>(a.blksize);
  st.st_rdev = a.rdev;
  st.st_atim = to_timespec(a.atime_ns);
  st.st_mtim = to_timespec(a.mtime_ns);
  st.st_ctim = to_timespec(a.ctime_ns);
  return st;
}

fuse_entry_param to_entry(const Attr& a) {
  fuse_entry_param e{};
  e.ino = a.ino;
  e.generation = 1;  // inode numbers are never reused while mounted
  e.attr = to_stat(a);
  e.attr_timeout = kTimeout;
  e.entry_timeout = kTimeout;
  return e;
}

Caller caller_of(fuse_req_t req) {
  const fuse_ctx* c = fuse_req_ctx(req);
  return {c->uid, c->gid};
}

void reply_sized(fuse_req_t req, std::size_t size, const void* data, std::size_t length) {
  if (size == 0) {
    fuse_reply_xattr(req, length);  // the caller is asking how big a buffer to use
  } else if (length > size) {
    fuse_reply_err(req, ERANGE);
  } else {
    fuse_reply_buf(req, static_cast<const char*>(data), length);
  }
}

// ---- operations ------------------------------------------------------------

void op_init(void*, fuse_conn_info* conn) {
  // The kernel gathers small writes into larger ones: essential for speed,
  // and Nanoer compresses whole chunks anyway.
  if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
  conn->max_write = 1 << 20;
  conn->max_readahead = 1 << 20;
}

void op_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    fuse_entry_param e = to_entry(fs_of(req).lookup(parent, name));
    fuse_reply_entry(req, &e);
  });
}

void op_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  try {
    fs_of(req).forget(ino, nlookup);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "nanoerfs: forget: %s\n", e.what());
  }
  fuse_reply_none(req);
}

void op_forget_multi(fuse_req_t req, size_t count, fuse_forget_data* forgets) {
  for (size_t i = 0; i < count; ++i) {
    try {
      fs_of(req).forget(forgets[i].ino, forgets[i].nlookup);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "nanoerfs: forget: %s\n", e.what());
    }
  }
  fuse_reply_none(req);
}

void op_getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info*) {
  guarded(req, [&] {
    struct stat st = to_stat(fs_of(req).getattr(ino));
    fuse_reply_attr(req, &st, kTimeout);
  });
}

void op_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, fuse_file_info*) {
  guarded(req, [&] {
    SetAttrRequest r;
    if (to_set & FUSE_SET_ATTR_MODE) r.mode = attr->st_mode;
    if (to_set & FUSE_SET_ATTR_UID) r.uid = attr->st_uid;
    if (to_set & FUSE_SET_ATTR_GID) r.gid = attr->st_gid;
    if (to_set & FUSE_SET_ATTR_SIZE) r.size = static_cast<std::uint64_t>(attr->st_size);
    if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
      r.atime = SetAttrRequest::Time::now;
    } else if (to_set & FUSE_SET_ATTR_ATIME) {
      r.atime = SetAttrRequest::Time::set;
      r.atime_ns = from_timespec(attr->st_atim);
    }
    if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
      r.mtime = SetAttrRequest::Time::now;
    } else if (to_set & FUSE_SET_ATTR_MTIME) {
      r.mtime = SetAttrRequest::Time::set;
      r.mtime_ns = from_timespec(attr->st_mtim);
    }
    struct stat st = to_stat(fs_of(req).setattr(ino, r));
    fuse_reply_attr(req, &st, kTimeout);
  });
}

void op_readlink(fuse_req_t req, fuse_ino_t ino) {
  guarded(req, [&] {
    std::string target = fs_of(req).readlink(ino);
    fuse_reply_readlink(req, target.c_str());
  });
}

void op_mknod(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, dev_t rdev) {
  guarded(req, [&] {
    fuse_entry_param e = to_entry(fs_of(req).mknod(parent, name, mode, rdev, caller_of(req)));
    fuse_reply_entry(req, &e);
  });
}

void op_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
  guarded(req, [&] {
    fuse_entry_param e = to_entry(fs_of(req).mkdir(parent, name, mode, caller_of(req)));
    fuse_reply_entry(req, &e);
  });
}

void op_symlink(fuse_req_t req, const char* target, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    fuse_entry_param e = to_entry(fs_of(req).symlink(parent, name, target, caller_of(req)));
    fuse_reply_entry(req, &e);
  });
}

void op_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t new_parent, const char* new_name) {
  guarded(req, [&] {
    fuse_entry_param e = to_entry(fs_of(req).link(ino, new_parent, new_name));
    fuse_reply_entry(req, &e);
  });
}

void op_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    fs_of(req).unlink(parent, name);
    fuse_reply_err(req, 0);
  });
}

void op_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    fs_of(req).rmdir(parent, name);
    fuse_reply_err(req, 0);
  });
}

void op_rename(fuse_req_t req, fuse_ino_t parent, const char* name, fuse_ino_t new_parent,
               const char* new_name, unsigned flags) {
  guarded(req, [&] {
    fs_of(req).rename(parent, name, new_parent, new_name, flags);
    fuse_reply_err(req, 0);
  });
}

void op_open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
  guarded(req, [&] {
    fs_of(req).open(ino);
    fi->keep_cache = 1;  // only this process changes the files: cached pages stay valid
    fuse_reply_open(req, fi);
  });
}

void op_create(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, fuse_file_info* fi) {
  guarded(req, [&] {
    Filesystem& fs = fs_of(req);
    fuse_entry_param e = to_entry(fs.mknod(parent, name, S_IFREG | (mode & 07777), 0, caller_of(req)));
    fs.open(e.ino);
    fi->keep_cache = 1;
    fuse_reply_create(req, &e, fi);
  });
}

void op_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info*) {
  guarded(req, [&] {
    thread_local Bytes buffer;
    buffer.resize(size);
    std::size_t n = fs_of(req).read(ino, static_cast<std::uint64_t>(off), buffer);
    fuse_reply_buf(req, reinterpret_cast<const char*>(buffer.data()), n);
  });
}

void op_write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off,
              fuse_file_info*) {
  guarded(req, [&] {
    std::size_t n = fs_of(req).write(ino, static_cast<std::uint64_t>(off),
                                     ByteView(reinterpret_cast<const std::uint8_t*>(buf), size));
    fuse_reply_write(req, n);
  });
}

void op_flush(fuse_req_t req, fuse_ino_t, fuse_file_info*) { fuse_reply_err(req, 0); }

void op_release(fuse_req_t req, fuse_ino_t ino, fuse_file_info*) {
  guarded(req, [&] {
    fs_of(req).release(ino);
    fuse_reply_err(req, 0);
  });
}

void op_fsync(fuse_req_t req, fuse_ino_t ino, int, fuse_file_info*) {
  guarded(req, [&] {
    fs_of(req).fsync(ino);
    fuse_reply_err(req, 0);
  });
}

void op_opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
  guarded(req, [&] {
    // A snapshot, so concurrent changes cannot make a listing skip or repeat names.
    auto entries = std::make_unique<std::vector<DirEntry>>(fs_of(req).list(ino));
    fi->fh = reinterpret_cast<std::uint64_t>(entries.get());
    if (fuse_reply_open(req, fi) == 0) entries.release();
  });
}

std::vector<DirEntry>& entries_of(fuse_file_info* fi) {
  return *reinterpret_cast<std::vector<DirEntry>*>(fi->fh);
}

void op_readdir(fuse_req_t req, fuse_ino_t, size_t size, off_t off, fuse_file_info* fi) {
  guarded(req, [&] {
    const auto& entries = entries_of(fi);
    std::vector<char> buf(size);
    std::size_t used = 0;
    for (std::size_t i = static_cast<std::size_t>(off); i < entries.size(); ++i) {
      struct stat st {};
      st.st_ino = entries[i].ino;
      st.st_mode = entries[i].mode;
      std::size_t n = fuse_add_direntry(req, buf.data() + used, size - used, entries[i].name.c_str(),
                                        &st, static_cast<off_t>(i + 1));
      if (n > size - used) break;
      used += n;
    }
    fuse_reply_buf(req, buf.data(), used);
  });
}

void op_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi) {
  guarded(req, [&] {
    Filesystem& fs = fs_of(req);
    const auto& entries = entries_of(fi);
    std::vector<char> buf(size);
    std::size_t used = 0;
    for (std::size_t i = static_cast<std::size_t>(off); i < entries.size(); ++i) {
      const DirEntry& d = entries[i];
      // Check it fits before looking it up: a lookup counts, and an entry
      // that is not returned must not be counted.
      std::size_t need = fuse_add_direntry_plus(req, nullptr, 0, d.name.c_str(), nullptr, 0);
      if (need > size - used) break;
      fuse_entry_param e{};
      try {
        bool dot = d.name == "." || d.name == "..";
        // "." and ".." are not counted as lookups (libfuse documentation).
        e = to_entry(dot ? fs.getattr(d.ino) : fs.lookup(ino, d.name));
      } catch (const Error& err) {
        if (err.to_errno() == ENOENT) continue;  // removed since opendir
        throw;
      }
      used += fuse_add_direntry_plus(req, buf.data() + used, size - used, d.name.c_str(), &e,
                                     static_cast<off_t>(i + 1));
    }
    fuse_reply_buf(req, buf.data(), used);
  });
}

void op_releasedir(fuse_req_t req, fuse_ino_t, fuse_file_info* fi) {
  delete &entries_of(fi);
  fuse_reply_err(req, 0);
}

void op_fsyncdir(fuse_req_t req, fuse_ino_t, int, fuse_file_info*) {
  guarded(req, [&] {
    fs_of(req).sync();
    fuse_reply_err(req, 0);
  });
}

void op_statfs(fuse_req_t req, fuse_ino_t) {
  guarded(req, [&] {
    FsStats s = fs_of(req).statfs();
    struct statvfs st {};
    st.f_bsize = s.block_size;
    st.f_frsize = s.block_size;
    st.f_blocks = s.total_blocks;
    st.f_bfree = s.free_blocks;
    st.f_bavail = s.available_blocks;
    st.f_files = s.files + s.free_files;
    st.f_ffree = s.free_files;
    st.f_favail = s.free_files;
    st.f_namemax = s.name_max;
    fuse_reply_statfs(req, &st);
  });
}

void op_setxattr(fuse_req_t req, fuse_ino_t ino, const char* name, const char* value, size_t size,
                 int flags) {
  guarded(req, [&] {
    fs_of(req).setxattr(ino, name, ByteView(reinterpret_cast<const std::uint8_t*>(value), size),
                        flags);
    fuse_reply_err(req, 0);
  });
}

void op_getxattr(fuse_req_t req, fuse_ino_t ino, const char* name, size_t size) {
  guarded(req, [&] {
    Bytes value = fs_of(req).getxattr(ino, name);
    reply_sized(req, size, value.data(), value.size());
  });
}

void op_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  guarded(req, [&] {
    std::string names;
    for (const std::string& n : fs_of(req).listxattr(ino)) {
      names += n;
      names.push_back('\0');
    }
    reply_sized(req, size, names.data(), names.size());
  });
}

void op_removexattr(fuse_req_t req, fuse_ino_t ino, const char* name) {
  guarded(req, [&] {
    fs_of(req).removexattr(ino, name);
    fuse_reply_err(req, 0);
  });
}

void op_access(fuse_req_t req, fuse_ino_t, int) {
  fuse_reply_err(req, 0);  // permissions are checked by the kernel (default_permissions)
}

void op_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
                  fuse_file_info*) {
  guarded(req, [&] {
    if (offset < 0 || length <= 0) throw_posix(EINVAL);
    fs_of(req).fallocate(ino, mode, static_cast<std::uint64_t>(offset),
                         static_cast<std::uint64_t>(length));
    fuse_reply_err(req, 0);
  });
}

fuse_lowlevel_ops make_ops() {
  fuse_lowlevel_ops ops{};
  ops.init = op_init;
  ops.lookup = op_lookup;
  ops.forget = op_forget;
  ops.forget_multi = op_forget_multi;
  ops.getattr = op_getattr;
  ops.setattr = op_setattr;
  ops.readlink = op_readlink;
  ops.mknod = op_mknod;
  ops.mkdir = op_mkdir;
  ops.symlink = op_symlink;
  ops.link = op_link;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.rename = op_rename;
  ops.open = op_open;
  ops.create = op_create;
  ops.read = op_read;
  ops.write = op_write;
  ops.flush = op_flush;
  ops.release = op_release;
  ops.fsync = op_fsync;
  ops.opendir = op_opendir;
  ops.readdir = op_readdir;
  ops.readdirplus = op_readdirplus;
  ops.releasedir = op_releasedir;
  ops.fsyncdir = op_fsyncdir;
  ops.statfs = op_statfs;
  ops.setxattr = op_setxattr;
  ops.getxattr = op_getxattr;
  ops.listxattr = op_listxattr;
  ops.removexattr = op_removexattr;
  ops.access = op_access;
  ops.fallocate = op_fallocate;
  // No lseek: with the writeback cache, data just written can still be in
  // the kernel's page cache, so Nanoer would report a hole where there is
  // data, and tools that skip holes (cp, tar) would lose it. Without the
  // operation the kernel treats the whole file as data, which is always safe.
  return ops;
}

// ---- command line ----------------------------------------------------------

void usage(std::ostream& out) {
  out << "Usage: nanoerfs [options] IMAGE MOUNTPOINT\n"
         "       mount -t nanoer [-o options] IMAGE MOUNTPOINT\n"
         "\n"
         "Mount the Nanoer filesystem on IMAGE (an image file or block device).\n"
         "\n"
         "Options:\n"
         "  -o safe_space=SIZE   decompressed-chunk cache (default 256M)\n"
         "  -o metadata_cache=SIZE  cache of metadata tree nodes (default 64M)\n"
         "  -o commit=SECONDS    commit interval (default 5)\n"
         "  -o ro                read-only\n"
         "  -o nodedup           do not deduplicate new data\n"
         "  -o nodiscard         do not give freed space back (holes, TRIM)\n"
         "  -o cold_after=AGE    recompress data of files unchanged this long (7d, 12h, 30m,\n"
         "                       90s; never: not at all). Default 7d\n"
         "  -o idle=SECONDS      background work (garbage collection, recompressing cold\n"
         "                       data) after this long without changes (default 10)\n"
         "  -o allow_other       let other users access the mount\n"
         "  -o OPTION            other FUSE or mount options\n"
         "  -f                   stay in the foreground\n"
         "  -d                   FUSE debug output (implies -f)\n"
         "  -h, --help           show this help\n";
}

std::optional<std::uint64_t> parse_size(std::string_view text) {
  std::uint64_t n = 0;
  std::size_t i = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) n = n * 10 + static_cast<std::uint64_t>(text[i] - '0');
  if (i == 0) return std::nullopt;
  std::string_view unit = text.substr(i);
  if (unit.empty()) return n;
  switch (unit[0] | 0x20) {
    case 'k': return n << 10;
    case 'm': return n << 20;
    case 'g': return n << 30;
    default: return std::nullopt;
  }
}

// Memory freed by the cache, the dedup index and the request threads goes
// back to the system (docs/M6_PLAN.md, done-when 1). glibc gives each thread
// that allocates its own arena (up to 8 per CPU) and keeps what is freed in
// it: with 16 request threads, a mount of 1M files held 1.4 GiB where the
// budgets come to 512 MiB. Two arenas, and trimming them every 2 seconds,
// keep it near 600 MiB, and reads no slower (KNOWLEDGE.md §14).
constexpr int kArenas = 2;
constexpr auto kTrimInterval = std::chrono::seconds(2);

std::jthread start_trimming() {
  return std::jthread([](std::stop_token stop) {
    std::mutex m;
    std::condition_variable_any wake;
    std::unique_lock lock(m);
    while (!stop.stop_requested()) {
      wake.wait_for(lock, stop, kTrimInterval, [] { return false; });  // woken only by stop
      malloc_trim(0);
    }
  });
}

// Escapes a value for a FUSE -o option list.
std::string escape_option(std::string_view value) {
  std::string out;
  for (char c : value) {
    if (c == ',' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  mallopt(M_ARENA_MAX, kArenas);
  std::vector<std::string_view> args(argv + 1, argv + argc);
  std::vector<std::string> positional;
  std::vector<std::string> fuse_options = {"default_permissions", "subtype=nanoer"};
  Filesystem::Options options;
  options.start_threads = false;  // after daemonizing: threads do not survive fork()
  bool foreground = false;
  bool debug = false;

  auto take_options = [&](std::string_view list) -> bool {
    std::size_t start = 0;
    while (start <= list.size()) {
      std::size_t comma = list.find(',', start);
      std::string_view opt = list.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
      start = comma == std::string_view::npos ? list.size() + 1 : comma + 1;
      if (opt.empty()) continue;
      if (opt.starts_with("safe_space=")) {
        auto v = parse_size(opt.substr(11));
        if (!v) return false;
        options.safe_space = *v;
      } else if (opt.starts_with("commit=")) {
        auto v = parse_size(opt.substr(7));
        if (!v) return false;
        options.commit_interval_s = static_cast<unsigned>(*v);
      } else if (opt.starts_with("cold_after=")) {
        auto v = parse_duration(opt.substr(11));
        if (!v) return false;
        options.cold_after_s = *v;
      } else if (opt.starts_with("idle=")) {
        auto v = parse_size(opt.substr(5));
        if (!v) return false;
        options.gc_idle_s = static_cast<unsigned>(*v);
      } else if (opt == "nodedup") {
        options.dedup = false;
      } else if (opt == "nodiscard") {
        options.discard = false;
      } else if (opt.starts_with("metadata_cache=")) {
        auto v = parse_size(opt.substr(15));
        if (!v || *v < (1 << 20)) return false;
        options.metadata_cache = *v;
      } else if (opt == "ro") {
        options.read_only = true;
        fuse_options.emplace_back("ro");
      } else if (opt == "rw" || opt == "defaults" || opt == "auto" || opt == "noauto" ||
                 opt == "user" || opt == "users" || opt == "nofail" || opt == "_netdev" ||
                 opt.starts_with("x-") || opt == "relatime" || opt == "noatime") {
        // fstab and mount(8) options with no meaning here (atime is never
        // updated on reads anyway).
      } else {
        fuse_options.emplace_back(opt);
      }
    }
    return true;
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "-h" || a == "--help") {
      usage(std::cout);
      return 0;
    } else if (a == "-f") {
      foreground = true;
    } else if (a == "-d") {
      debug = foreground = true;
    } else if (a == "-o") {
      if (i + 1 >= args.size() || !take_options(args[++i])) {
        std::cerr << "nanoerfs: bad -o option\n";
        return 1;
      }
    } else if (a.starts_with("-o")) {
      if (!take_options(a.substr(2))) {
        std::cerr << "nanoerfs: bad -o option\n";
        return 1;
      }
    } else if (a == "-n" || a == "-s" || a == "-v") {
      // Passed by mount(8) to mount helpers; nothing to do.
    } else if (!a.empty() && a[0] == '-') {
      usage(std::cerr);
      return 1;
    } else {
      positional.emplace_back(a);
    }
  }
  if (positional.size() != 2) {
    usage(std::cerr);
    return 1;
  }
  const std::string& image = positional[0];
  const std::string& mountpoint = positional[1];

  Context context;
  try {
    context.store = FileStore::open(image, options.read_only ? FileStore::Mode::read_only
                                                             : FileStore::Mode::read_write);
    context.fs = Filesystem::mount(*context.store, options);
  } catch (const Error& e) {
    std::cerr << "nanoerfs: cannot mount " << image << ": " << e.what() << "\n";
    return 1;
  }
  if (context.fs->read_only() && !options.read_only) fuse_options.emplace_back("ro");

  std::error_code ec;
  std::string fsname = std::filesystem::absolute(image, ec).string();
  fuse_options.push_back("fsname=" + escape_option(ec ? image : fsname));
  std::string joined;
  for (const std::string& o : fuse_options) joined += (joined.empty() ? "" : ",") + o;

  std::vector<std::string> fuse_argv = {"nanoerfs", "-o", joined};
  if (debug) fuse_argv.emplace_back("-d");
  std::vector<char*> fuse_args_ptrs;
  for (auto& s : fuse_argv) fuse_args_ptrs.push_back(s.data());
  fuse_args fargs = FUSE_ARGS_INIT(static_cast<int>(fuse_args_ptrs.size()), fuse_args_ptrs.data());

  fuse_lowlevel_ops ops = make_ops();
  fuse_session* se = fuse_session_new(&fargs, &ops, sizeof ops, &context);
  if (!se) {
    std::cerr << "nanoerfs: cannot start a FUSE session\n";
    return 1;
  }
  int rc = 1;
  if (fuse_set_signal_handlers(se) == 0) {
    if (fuse_session_mount(se, mountpoint.c_str()) == 0) {
      if (fuse_daemonize(foreground) == 0) {
        context.fs->start();
        std::jthread trimming = start_trimming();
        fuse_loop_config* config = fuse_loop_cfg_create();
        fuse_loop_cfg_set_clone_fd(config, 0);
        fuse_loop_cfg_set_max_threads(config, 16);
        rc = fuse_session_loop_mt(se, config) == 0 ? 0 : 1;
        fuse_loop_cfg_destroy(config);
      }
      fuse_session_unmount(se);
    }
    fuse_remove_signal_handlers(se);
  }
  fuse_session_destroy(se);
  fuse_opt_free_args(&fargs);
  try {
    context.fs->unmount();  // final commit and checkpoint
  } catch (const Error& e) {
    std::cerr << "nanoerfs: unmount failed: " << e.what() << "\n";
    rc = 1;
  }
  return rc;
}
