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

/** WHATWG TextEncoderEncodeIntoResult (read = UTF-16 code units of source). */
struct EncodeIntoResult {
  std::size_t read{0};
  std::size_t written{0};
};

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
   * In-place encode into a caller buffer (TextEncoder.encodeInto).
   *
   * Never writes a partial multi-byte UTF-8 sequence: if dest is too small for
   * the next code point, that code point is omitted (trim_partial_utf8).
   * `read` is the UTF-16 code-unit length of the encoded prefix (WHATWG).
   * `written` is the number of bytes stored in dest.
   */
  [[nodiscard]] static Result<EncodeIntoResult> encode_into(
      std::string_view utf8,
      std::span<std::uint8_t> dest) {
    if (!utf8.empty() &&
        !simdutf::validate_utf8(utf8.data(), utf8.size())) {
      return std::unexpected(Error{"TextEncoder.encodeInto: invalid UTF-8"});
    }
    std::size_t n = std::min(utf8.size(), dest.size());
    if (n > 0 && n < utf8.size()) {
      // Prefix of valid UTF-8 may end mid-sequence — peel incomplete tail.
      n = simdutf::trim_partial_utf8(utf8.data(), n);
    }
    if (n > 0) {
      std::memcpy(dest.data(), utf8.data(), n);
    }
    const std::size_t read =
        n == 0 ? 0 : simdutf::utf16_length_from_utf8(utf8.data(), n);
    return EncodeIntoResult{read, n};
  }
};

}  // namespace vacps::text
