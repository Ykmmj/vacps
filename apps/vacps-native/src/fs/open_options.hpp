#pragma once

/**
 * Open modes and options for vacps::fs::File.
 *
 * Primary public API is string OpenMode ("read" | "read-write" | …).
 * Mapping OpenMode → POSIX open bits happens inside File::open.
 */

#include "app/error.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace vacps::fs {

/**
 * File open modes — primary C++ / JS API.
 *
 * Mapping (POSIX):
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

/**
 * Backend selection for File::open.
 * Auto: Asio/io_uring when the process probe succeeded and an executor is
 * provided; otherwise POSIX pool backend.
 */
enum class FileBackend {
  Auto,
  Asio,
  Posix,
};

struct OpenOptions {
  OpenMode mode{OpenMode::read};
  /**
   * Permission bits when the mode creates the file (open(2) mode).
   * Default 0644 when unset. Applied on the single POSIX open path for both
   * backends.
   */
  std::optional<std::uint32_t> permissions{};
};

/** Parse JS/API mode string → OpenMode ("read", "read-write", …). */
[[nodiscard]] Result<OpenMode> open_mode_from_string(std::string_view s);

/** Stable string form matching JS FileOpenMode. */
[[nodiscard]] const char* open_mode_to_string(OpenMode mode) noexcept;

/** Effective create mode bits (default 0644). */
[[nodiscard]] inline unsigned effective_permissions(
    const OpenOptions& opts) noexcept {
  return opts.permissions.value_or(0644u);
}

/** True when OpenMode implies O_CREAT. */
[[nodiscard]] bool open_mode_creates(OpenMode mode) noexcept;

/** True when OpenMode implies O_APPEND. */
[[nodiscard]] bool open_mode_appends(OpenMode mode) noexcept;

/**
 * POSIX open(2) flags for OpenMode (without O_CLOEXEC).
 * Single source of truth for both backends.
 */
[[nodiscard]] int posix_open_flags(OpenMode mode) noexcept;

}  // namespace vacps::fs
