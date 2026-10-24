#include "nanoer/error.h"

#include <cerrno>
#include <cstring>

#include "nanoer/bytes.h"

namespace nanoer {

const char* errc_name(Errc code) {
  switch (code) {
    case Errc::io: return "I/O error";
    case Errc::corrupt: return "corrupt data";
    case Errc::invalid_argument: return "invalid argument";
    case Errc::unsupported: return "unsupported";
    case Errc::no_space: return "no space left";
    case Errc::busy: return "busy";
    case Errc::posix: return "filesystem error";
  }
  return "unknown error";
}

Error::Error(Errc code, const std::string& message, int sys_errno)
    : std::runtime_error(message), code_(code), sys_errno_(sys_errno) {}

int Error::to_errno() const noexcept {
  switch (code_) {
    case Errc::io: return sys_errno_ ? sys_errno_ : EIO;
    case Errc::corrupt: return EUCLEAN;  // what Btrfs and ext4 report for checksum failures
    case Errc::invalid_argument: return EINVAL;
    case Errc::unsupported: return EOPNOTSUPP;
    case Errc::no_space: return ENOSPC;
    case Errc::busy: return EBUSY;
    case Errc::posix: return sys_errno_ ? sys_errno_ : EIO;
  }
  return EIO;
}

void throw_posix(int errno_value, const std::string& message) {
  throw Error(Errc::posix, message.empty() ? std::strerror(errno_value) : message, errno_value);
}

std::string to_hex(ByteView data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (std::uint8_t b : data) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0xf]);
  }
  return out;
}

}  // namespace nanoer
