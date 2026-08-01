#include "fs/open_options.hpp"

#include <format>

namespace vacps::fs {

Result<OpenMode> open_mode_from_string(std::string_view s) {
  if (s == "read") return OpenMode::read;
  if (s == "read-write") return OpenMode::read_write;
  if (s == "write") return OpenMode::write;
  if (s == "write-new") return OpenMode::write_new;
  if (s == "append") return OpenMode::append;
  if (s == "append-read") return OpenMode::append_read;
  return std::unexpected(Error{std::format(
      "File.open: unknown mode \"{}\" (expected read|read-write|write|write-new|append|append-read)",
      s)});
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

Flags flags_for_open_mode(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::read:
      return Flags::read_only;
    case OpenMode::read_write:
      return Flags::read_write;
    case OpenMode::write:
      return Flags::write_only | Flags::create | Flags::truncate;
    case OpenMode::write_new:
      return Flags::write_only | Flags::create | Flags::exclusive;
    case OpenMode::append:
      return Flags::write_only | Flags::create | Flags::append;
    case OpenMode::append_read:
      return Flags::read_write | Flags::create | Flags::append;
  }
  return Flags::read_only;
}

bool flags_create(Flags flags) noexcept {
#if defined(BOOST_ASIO_HAS_FILE)
  return (static_cast<unsigned>(flags) &
          static_cast<unsigned>(asio::file_base::create)) != 0;
#else
  return (static_cast<unsigned>(flags) & static_cast<unsigned>(Flags::create)) != 0;
#endif
}

}  // namespace vacps::fs
