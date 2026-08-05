#include "fs/open_options.hpp"

#include <format>

#if defined(__linux__)
#include <fcntl.h>
#endif

namespace vacps::fs {

Result<OpenMode> open_mode_from_string(std::string_view s) {
  if (s == "read") return OpenMode::read;
  if (s == "read-write") return OpenMode::read_write;
  if (s == "write") return OpenMode::write;
  if (s == "write-new") return OpenMode::write_new;
  if (s == "append") return OpenMode::append;
  if (s == "append-read") return OpenMode::append_read;
  return std::unexpected(Error{
      std::format(
          "File.open: unknown mode \"{}\" (expected "
          "read|read-write|write|write-new|append|append-read)",
          s),
      "open",
      0});
}

const char* open_mode_to_string(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::read:
      return "read";
    case OpenMode::read_write:
      return "read-write";
    case OpenMode::write:
      return "write";
    case OpenMode::write_new:
      return "write-new";
    case OpenMode::append:
      return "append";
    case OpenMode::append_read:
      return "append-read";
  }
  return "read";
}

bool open_mode_creates(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::read:
    case OpenMode::read_write:
      return false;
    case OpenMode::write:
    case OpenMode::write_new:
    case OpenMode::append:
    case OpenMode::append_read:
      return true;
  }
  return false;
}

bool open_mode_appends(OpenMode mode) noexcept {
  return mode == OpenMode::append || mode == OpenMode::append_read;
}

int posix_open_flags(OpenMode mode) noexcept {
#if defined(__linux__)
  switch (mode) {
    case OpenMode::read:
      return O_RDONLY;
    case OpenMode::read_write:
      return O_RDWR;
    case OpenMode::write:
      return O_WRONLY | O_CREAT | O_TRUNC;
    case OpenMode::write_new:
      return O_WRONLY | O_CREAT | O_EXCL;
    case OpenMode::append:
      return O_WRONLY | O_CREAT | O_APPEND;
    case OpenMode::append_read:
      return O_RDWR | O_CREAT | O_APPEND;
  }
  return O_RDONLY;
#else
  (void)mode;
  return 0;
#endif
}

}  // namespace vacps::fs
