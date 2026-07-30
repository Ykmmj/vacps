#pragma once

#include <expected>
#include <string>
#include <utility>

namespace vacps {

/** Lightweight error for Result/expected. Move-only friendly; cheap to return. */
struct Error {
  std::string message;

  Error() = default;
  explicit Error(std::string msg) : message(std::move(msg)) {}

  static Error from_errno_msg(std::string msg) { return Error{std::move(msg)}; }
};

template <class T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

/** Successful void result. Prefer `return {}` in member functions (avoids `ok()` name clash). */
inline VoidResult success() { return {}; }

inline Error err(std::string message) { return Error{std::move(message)}; }

}  // namespace vacps
