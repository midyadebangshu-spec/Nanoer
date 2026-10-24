// Error reporting.
//
// The engine throws nanoer::Error. The FUSE adapter (M2) catches it at the
// boundary and turns it into an errno for the kernel; see Error::to_errno().
#pragma once

#include <stdexcept>
#include <string>

namespace nanoer {

enum class Errc {
  io,                // the backing store failed (errno() has the details)
  corrupt,           // on-disk data failed a check; never return it
  invalid_argument,  // the caller asked for something impossible
  unsupported,       // valid data using a feature this build does not know
  no_space,          // the store is full
  busy,              // the store is in use by another process
};

const char* errc_name(Errc code);

class Error : public std::runtime_error {
 public:
  Error(Errc code, const std::string& message, int sys_errno = 0);

  Errc code() const noexcept { return code_; }
  int sys_errno() const noexcept { return sys_errno_; }

  // The errno a filesystem should report for this error.
  int to_errno() const noexcept;

 private:
  Errc code_;
  int sys_errno_;
};

}  // namespace nanoer
