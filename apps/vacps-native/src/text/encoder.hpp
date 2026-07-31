#pragma once

#include "app/error.hpp"

#include <simdutf.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace vacps::text {

/**
 * WHATWG TextEncoder domain helper (UTF-8 only).
 *
 * Stateless — no heap identity required. Bindings may use a JS class for
 * identity alone and call these free/static methods.
 *
 * Input is already UTF-8 (QuickJS `JS_ToCString`); we validate with simdutf
 * and return the byte sequence.
 */
class Encoder final {
 public:
  Encoder() = default;

  [[nodiscard]] static constexpr std::string_view encoding() noexcept {
    return "utf-8";
  }

  /**
   * Encode a UTF-8 string to bytes.
   * Fails if `utf8` is not well-formed UTF-8 (should not happen from QuickJS).
   */
  [[nodiscard]] static Result<std::vector<std::uint8_t>> encode(
      std::string_view utf8) {
    if (!utf8.empty() &&
        !simdutf::validate_utf8(utf8.data(), utf8.size())) {
      return std::unexpected(Error{"TextEncoder.encode: invalid UTF-8"});
    }
    std::vector<std::uint8_t> out(utf8.size());
    if (!utf8.empty()) {
      std::memcpy(out.data(), utf8.data(), utf8.size());
    }
    return out;
  }

  /**
   * In-place encode into a caller buffer (TextEncoder.encodeInto subset).
   * Writes min(utf8.size(), dest.size()) bytes after validation.
   * Returns number of bytes written.
   */
  [[nodiscard]] static Result<std::size_t> encode_into(
      std::string_view utf8,
      std::span<std::uint8_t> dest) {
    if (!utf8.empty() &&
        !simdutf::validate_utf8(utf8.data(), utf8.size())) {
      return std::unexpected(Error{"TextEncoder.encodeInto: invalid UTF-8"});
    }
    const std::size_t n = std::min(utf8.size(), dest.size());
    if (n > 0) {
      std::memcpy(dest.data(), utf8.data(), n);
    }
    return n;
  }
};

}  // namespace vacps::text
