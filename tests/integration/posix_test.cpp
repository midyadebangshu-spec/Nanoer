// POSIX behaviour through a real FUSE mount. Run by run_mounted.sh, which
// mounts a fresh filesystem and sets NANOER_TEST_DIR to the mountpoint.
// Uses only system calls, so it would pass on ext4 too: that is the point.

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {

std::string root_dir() {
  const char* d = std::getenv("NANOER_TEST_DIR");
  return d ? d : "";
}

class Posix : public ::testing::Test {
 protected:
  void SetUp() override {
    if (root_dir().empty()) GTEST_SKIP() << "NANOER_TEST_DIR is not set";
    dir_ = root_dir() + "/" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    ASSERT_EQ(::mkdir(dir_.c_str(), 0777), 0) << std::strerror(errno);
  }
  std::string path(const std::string& name) const { return dir_ + "/" + name; }

  void write_file(const std::string& name, const std::string& data) {
    int fd = ::open(path(name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    ASSERT_EQ(::write(fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
    ASSERT_EQ(::close(fd), 0);
  }
  std::string read_file(const std::string& name) {
    std::ifstream in(path(name), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }
  struct stat stat_of(const std::string& name) {
    struct stat st {};
    EXPECT_EQ(::lstat(path(name).c_str(), &st), 0) << name << ": " << std::strerror(errno);
    return st;
  }
  int err(int result) { return result == 0 ? 0 : errno; }

  std::string dir_;
};

std::string random_text(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::string s(n, ' ');
  for (auto& c : s) c = static_cast<char>('a' + rng() % 26);
  return s;
}

}  // namespace

TEST_F(Posix, CreateWriteReadAppendTruncate) {
  write_file("f", "hello");
  EXPECT_EQ(read_file("f"), "hello");
  int fd = ::open(path("f").c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::write(fd, " world", 6), 6);
  ::close(fd);
  EXPECT_EQ(read_file("f"), "hello world");
  ASSERT_EQ(::truncate(path("f").c_str(), 5), 0);
  EXPECT_EQ(read_file("f"), "hello");
  ASSERT_EQ(::truncate(path("f").c_str(), 8), 0);
  EXPECT_EQ(read_file("f"), std::string("hello\0\0\0", 8));
  fd = ::open(path("f").c_str(), O_WRONLY | O_TRUNC);
  ::close(fd);
  EXPECT_EQ(stat_of("f").st_size, 0);
  EXPECT_EQ(err(::open(path("f").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644)), EEXIST);
}

TEST_F(Posix, LargeAndSparseFiles) {
  std::string big = random_text(5 << 20, 1);
  write_file("big", big);
  EXPECT_EQ(read_file("big"), big);

  int fd = ::open(path("sparse").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(fd, 0);
  const off_t far = off_t{10} << 30;  // 10 GiB, on a much smaller filesystem
  ASSERT_EQ(::pwrite(fd, "x", 1, far), 1);
  struct stat st;
  ASSERT_EQ(::fstat(fd, &st), 0);
  EXPECT_EQ(st.st_size, far + 1);
  EXPECT_LT(st.st_blocks, 2048);  // only one chunk is stored
  char c = 1;
  ASSERT_EQ(::pread(fd, &c, 1, far / 2), 1);
  EXPECT_EQ(c, 0);
  // Holes may or may not be reported, but data must never be skipped.
  EXPECT_LE(::lseek(fd, 0, SEEK_DATA), far);
  EXPECT_GT(::lseek(fd, far, SEEK_HOLE), far);
  ::close(fd);
}

TEST_F(Posix, PunchHole) {
  std::string data = random_text(300000, 2);
  write_file("f", data);
  int fd = ::open(path("f").c_str(), O_RDWR);
  ASSERT_EQ(::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 1000, 200000), 0)
      << std::strerror(errno);
  ::close(fd);
  std::fill(data.begin() + 1000, data.begin() + 201000, '\0');
  EXPECT_EQ(read_file("f"), data);
}

TEST_F(Posix, SharedMmapWrites) {
  write_file("m", std::string(100000, 'a'));
  int fd = ::open(path("m").c_str(), O_RDWR);
  void* p = ::mmap(nullptr, 100000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED) << std::strerror(errno);
  std::memset(static_cast<char*>(p) + 50000, 'b', 10);
  ASSERT_EQ(::msync(p, 100000, MS_SYNC), 0);
  ::munmap(p, 100000);
  ::close(fd);
  std::string expected(100000, 'a');
  std::fill(expected.begin() + 50000, expected.begin() + 50010, 'b');
  EXPECT_EQ(read_file("m"), expected);
}

TEST_F(Posix, DeletedFileStaysUsableWhileOpen) {
  write_file("tmp", "still here");
  int fd = ::open(path("tmp").c_str(), O_RDWR);
  ASSERT_EQ(::unlink(path("tmp").c_str()), 0);
  EXPECT_EQ(err(::access(path("tmp").c_str(), F_OK)), ENOENT);
  char buf[16] = {};
  EXPECT_EQ(::pread(fd, buf, sizeof buf, 0), 10);
  EXPECT_STREQ(buf, "still here");
  EXPECT_EQ(::pwrite(fd, "X", 1, 0), 1);
  struct stat st;
  ASSERT_EQ(::fstat(fd, &st), 0);
  EXPECT_EQ(st.st_nlink, 0u);
  ::close(fd);
}

TEST_F(Posix, DirectoriesAndListing) {
  ASSERT_EQ(::mkdir(path("d").c_str(), 0755), 0);
  EXPECT_EQ(err(::mkdir(path("d").c_str(), 0755)), EEXIST);
  std::set<std::string> expected;
  for (int i = 0; i < 300; ++i) {
    std::string n = "file-" + std::to_string(i);
    write_file("d/" + n, "x");
    expected.insert(n);
  }
  ASSERT_EQ(::mkdir(path("d/sub").c_str(), 0755), 0);
  expected.insert("sub");
  DIR* d = ::opendir(path("d").c_str());
  ASSERT_NE(d, nullptr);
  std::set<std::string> seen;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    EXPECT_TRUE(seen.insert(n).second) << "listed twice: " << n;
  }
  ::closedir(d);
  EXPECT_EQ(seen, expected);
  EXPECT_EQ(stat_of("d").st_nlink, 3u);
  EXPECT_EQ(err(::rmdir(path("d").c_str())), ENOTEMPTY);
  EXPECT_EQ(err(::unlink(path("d").c_str())), EISDIR);
  EXPECT_EQ(err(::rmdir(path("d/file-1").c_str())), ENOTDIR);
  EXPECT_EQ(err(::mkdir(path(std::string(256, 'n')).c_str(), 0755)), ENAMETOOLONG);
}

TEST_F(Posix, Links) {
  write_file("orig", "data");
  ASSERT_EQ(::link(path("orig").c_str(), path("hard").c_str()), 0);
  EXPECT_EQ(stat_of("orig").st_nlink, 2u);
  EXPECT_EQ(stat_of("orig").st_ino, stat_of("hard").st_ino);
  ASSERT_EQ(::symlink("orig", path("soft").c_str()), 0);
  char target[64] = {};
  EXPECT_EQ(::readlink(path("soft").c_str(), target, sizeof target), 4);
  EXPECT_STREQ(target, "orig");
  EXPECT_TRUE(S_ISLNK(stat_of("soft").st_mode));
  EXPECT_EQ(read_file("soft"), "data");  // followed
  ASSERT_EQ(::unlink(path("orig").c_str()), 0);
  EXPECT_EQ(read_file("hard"), "data");
  EXPECT_EQ(stat_of("hard").st_nlink, 1u);
  struct stat st;
  EXPECT_EQ(::stat(path("soft").c_str(), &st), -1);  // dangling now
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(Posix, Rename) {
  write_file("a", "A");
  write_file("b", "B");
  ASSERT_EQ(::mkdir(path("d").c_str(), 0755), 0);
  ASSERT_EQ(::rename(path("a").c_str(), path("d/a2").c_str()), 0);
  EXPECT_EQ(read_file("d/a2"), "A");
  ASSERT_EQ(::rename(path("b").c_str(), path("d/a2").c_str()), 0);  // replaces
  EXPECT_EQ(read_file("d/a2"), "B");
  EXPECT_EQ(err(::rename(path("d").c_str(), path("d/inside").c_str())), EINVAL);

  write_file("x", "X");
  write_file("y", "Y");
  EXPECT_EQ(err(static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, path("x").c_str(), AT_FDCWD,
                                           path("y").c_str(), RENAME_NOREPLACE))),
            EEXIST);
  ASSERT_EQ(::syscall(SYS_renameat2, AT_FDCWD, path("x").c_str(), AT_FDCWD, path("y").c_str(),
                      RENAME_EXCHANGE),
            0);
  EXPECT_EQ(read_file("x"), "Y");
  EXPECT_EQ(read_file("y"), "X");
}

TEST_F(Posix, AttributesAndTimes) {
  write_file("f", "x");
  ASSERT_EQ(::chmod(path("f").c_str(), 0640), 0);
  EXPECT_EQ(stat_of("f").st_mode & 07777, 0640u);
  if (::geteuid() == 0) {
    ASSERT_EQ(::chown(path("f").c_str(), 1234, 5678), 0);
    EXPECT_EQ(stat_of("f").st_uid, 1234u);
    EXPECT_EQ(stat_of("f").st_gid, 5678u);
  }
  timespec times[2] = {{1000000000, 123456789}, {1500000000, 987654321}};
  ASSERT_EQ(::utimensat(AT_FDCWD, path("f").c_str(), times, 0), 0);
  struct stat st = stat_of("f");
  EXPECT_EQ(st.st_atim.tv_sec, 1000000000);
  EXPECT_EQ(st.st_atim.tv_nsec, 123456789);
  EXPECT_EQ(st.st_mtim.tv_sec, 1500000000);
  EXPECT_EQ(st.st_mtim.tv_nsec, 987654321);
  struct stat before = stat_of("f");
  ::usleep(20000);
  write_file("f", "changed");
  EXPECT_GT(stat_of("f").st_mtim.tv_sec * 1000000000L + stat_of("f").st_mtim.tv_nsec,
            before.st_mtim.tv_sec * 1000000000L + before.st_mtim.tv_nsec);
}

TEST_F(Posix, SpecialFiles) {
  ASSERT_EQ(::mkfifo(path("fifo").c_str(), 0600), 0);
  EXPECT_TRUE(S_ISFIFO(stat_of("fifo").st_mode));
  if (::geteuid() == 0) {
    ASSERT_EQ(::mknod(path("null").c_str(), S_IFCHR | 0666, makedev(1, 3)), 0);
    EXPECT_EQ(stat_of("null").st_rdev, makedev(1, 3));
  }
}

TEST_F(Posix, ExtendedAttributes) {
  write_file("f", "x");
  std::string file = path("f");
  ASSERT_EQ(::setxattr(file.c_str(), "user.colour", "blue", 4, 0), 0) << std::strerror(errno);
  char buf[16] = {};
  EXPECT_EQ(::getxattr(file.c_str(), "user.colour", nullptr, 0), 4);
  EXPECT_EQ(::getxattr(file.c_str(), "user.colour", buf, sizeof buf), 4);
  EXPECT_STREQ(buf, "blue");
  EXPECT_EQ(::getxattr(file.c_str(), "user.colour", buf, 2), -1);
  EXPECT_EQ(errno, ERANGE);
  EXPECT_EQ(::getxattr(file.c_str(), "user.none", buf, sizeof buf), -1);
  EXPECT_EQ(errno, ENODATA);
  EXPECT_EQ(err(::setxattr(file.c_str(), "user.colour", "red", 3, XATTR_CREATE)), EEXIST);
  char names[64] = {};
  ssize_t n = ::listxattr(file.c_str(), names, sizeof names);
  EXPECT_EQ(std::string(names, static_cast<std::size_t>(n)), std::string("user.colour\0", 12));
  ASSERT_EQ(::removexattr(file.c_str(), "user.colour"), 0);
  EXPECT_EQ(::listxattr(file.c_str(), nullptr, 0), 0);
}

TEST_F(Posix, PermissionsAreEnforced) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root to switch users";
  write_file("secret", "s");
  ASSERT_EQ(::chmod(path("secret").c_str(), 0600), 0);
  ASSERT_EQ(::chmod(dir_.c_str(), 0777), 0);
  pid_t pid = ::fork();
  if (pid == 0) {
    // As "nobody": may not read root's 0600 file, may create its own.
    if (::setgid(65534) || ::setuid(65534)) _exit(10);
    if (::open(path("secret").c_str(), O_RDONLY) >= 0 || errno != EACCES) _exit(11);
    int fd = ::open(path("mine").c_str(), O_WRONLY | O_CREAT, 0600);
    if (fd < 0) _exit(12);
    ::close(fd);
    _exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "10: setuid failed, 11: read allowed, 12: create failed";
  EXPECT_EQ(stat_of("mine").st_uid, 65534u);
}

TEST_F(Posix, SyncCallsSucceed) {
  write_file("f", "x");
  int fd = ::open(path("f").c_str(), O_RDWR);
  EXPECT_EQ(::fsync(fd), 0);
  EXPECT_EQ(::fdatasync(fd), 0);
  EXPECT_EQ(::syncfs(fd), 0);
  ::close(fd);
  int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
  EXPECT_EQ(::fsync(dfd), 0);
  ::close(dfd);
}

TEST_F(Posix, Statfs) {
  struct statvfs st;
  ASSERT_EQ(::statvfs(dir_.c_str(), &st), 0);
  EXPECT_EQ(st.f_bsize, 4096u);
  EXPECT_GT(st.f_blocks, 0u);
  EXPECT_LE(st.f_bavail, st.f_bfree);
  EXPECT_EQ(st.f_namemax, 255u);
}

TEST_F(Posix, ConcurrentWriters) {
  std::vector<pid_t> children;
  for (int c = 0; c < 4; ++c) {
    pid_t pid = ::fork();
    if (pid == 0) {
      std::string data = random_text(300000, static_cast<unsigned>(c));
      for (int round = 0; round < 10; ++round) {
        std::string name = path("w" + std::to_string(c) + "-" + std::to_string(round));
        int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || ::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) _exit(1);
        ::close(fd);
      }
      _exit(0);
    }
    children.push_back(pid);
  }
  for (pid_t pid : children) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  for (int c = 0; c < 4; ++c) {
    EXPECT_EQ(read_file("w" + std::to_string(c) + "-9"), random_text(300000, static_cast<unsigned>(c)));
  }
}
