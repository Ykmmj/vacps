#pragma once

/**
 * Open modes and options for vacps::fs::File.
 *
 * Primary public API is string OpenMode ("read" | "read-write" | …).
 * Mapping OpenMode → Asio Flags / POSIX open bits happens inside File::open
 * (domain), not in JS bindings.
 *
 * Flags (Asio file_base bitmask) remain an internal dual-backend detail.
 */

#include "app/error.hpp"

#include <boost/asio/detail/config.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

#if defined(BOOST_ASIO_HAS_FILE)
#include <boost/asio/file_base.hpp>
#endif

namespace vacps::fs {

namespace asio = boost::asio;

// ── Public open mode (JS string union) ────────────────────────────

/**
 * File open modes — primary C++ / JS API.
 *
 * Mapping (POSIX / Asio flags):
 * - read         → O_RDONLY
 * - read_write   → O_RDWR
 * - write        → O_WRONLY | O_CREAT | O_TRUNC
 * - write_new    → O_WRONLY | O_CREAT | O_EXCL
 * - append       → O_WRONLY | O_CREAT | O_APPEND
 * - append_read  → O_RDWR   | O_CREAT | O_APPEND
 */
enum class OpenMode {
  read,
  read_write,
  write,
  write_new,
  append,
  append_read,
};

struct OpenOptions {
  OpenMode mode{OpenMode::read};
  /**
   * Permission bits when the mode creates the file (pool-backend open(2)).
   * Default 0644 when unset. Asio random_access_file::open has no mode
   * parameter — library default applies on that backend.
   */
  std::optional<std::uint32_t> permissions{};
};

/** Parse JS/API mode string → OpenMode ("read", "read-write", …). */
[[nodiscard]] Result<OpenMode> open_mode_from_string(std::string_view s);

/** Stable string form matching JS FileOpenMode. */
[[nodiscard]] const char* open_mode_to_string(OpenMode mode) noexcept;

/** Effective create mode bits (default 0644). */
[[nodiscard]] inline unsigned effective_permissions(const OpenOptions& opts) noexcept {
  return opts.permissions.value_or(0644u);
}

/** True when OpenMode implies O_CREAT. */
[[nodiscard]] bool open_mode_creates(OpenMode mode) noexcept;

// ── Internal Flags (dual-backend Asio / POSIX bitmask) ────────────

#if defined(BOOST_ASIO_HAS_FILE)

using Flags = asio::file_base::flags;

#else

/**
 * Compatible bitmask when Asio file support is not compiled in.
 * Numeric values match Linux open flags. Not part of the JS surface.
 */
enum class Flags : int {
  read_only = 0,
  write_only = 1,
  read_write = 2,
  append = 1024,
  create = 64,
  exclusive = 128,
  truncate = 512,
  sync_all_on_write = 1052672,
};

inline Flags operator|(Flags a, Flags b) noexcept {
  return static_cast<Flags>(static_cast<int>(a) | static_cast<int>(b));
}
inline Flags operator&(Flags a, Flags b) noexcept {
  return static_cast<Flags>(static_cast<int>(a) & static_cast<int>(b));
}
inline Flags& operator|=(Flags& a, Flags b) noexcept {
  a = a | b;
  return a;
}

#endif

/** Map OpenMode → Asio/POSIX Flags (domain mapping for File::open). */
[[nodiscard]] Flags flags_for_open_mode(OpenMode mode) noexcept;

/** True when flags include create. */
[[nodiscard]] bool flags_create(Flags flags) noexcept;

[[nodiscard]] inline Flags flags_from_int(int bits) noexcept {
  return static_cast<Flags>(bits);
}

[[nodiscard]] inline int flags_to_int(Flags flags) noexcept {
  return static_cast<int>(flags);
}

}  // namespace vacps::fs
