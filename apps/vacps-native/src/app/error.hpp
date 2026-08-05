#pragma once

#include <expected>
#include <string>
#include <utility>

namespace vacps {

/**
 * Lightweight error for Result/expected.
 *
 * `message` is always populated for display / logging.
 * `operation` and `system_code` are optional structured fields for I/O and
 * other syscalls (e.g. operation="open", system_code=errno). Zero
 * system_code means "not a system error" / unavailable.
 */
struct Error {
  std::string message;
  std::string operation;
  int system_code{0};

  Error() = default;
  explicit Error(std::string msg) : message(std::move(msg)) {}
  Error(std::string msg, std::string op, int code = 0)
      : message(std::move(msg)),
        operation(std::move(op)),
        system_code(code) {}

  static Error from_errno_msg(std::string msg) { return Error{std::move(msg)}; }
};

template <class T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

/** Successful void result. Prefer `return {}` in member functions (avoids `ok()` name clash). */
inline VoidResult success() { return {}; }

inline Error err(std::string message) { return Error{std::move(message)}; }

}  // namespace vacps
