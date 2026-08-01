#pragma once

#include "app/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::text {

/** Encodings simdutf fully supports for TextDecoder v1. */
enum class Encoding {
  utf8,
  utf16le,
  utf16be,
};

struct DecoderOptions {
  bool fatal{false};
  /**
   * WHATWG ignoreBOM:
   * - false (default): consume a leading BOM matching the encoding
   * - true: keep BOM as U+FEFF character
   */
  bool ignore_bom{false};
};

/**
 * WHATWG TextDecoder domain object.
 *
 * Holds encoding label, fatal/ignoreBOM flags, and a streaming remainder
 * buffer for incomplete multi-byte sequences across `decode(..., stream=true)`.
 *
 * Does not implement the full legacy WHATWG encoding set — only utf-8 /
 * utf-16le / utf-16be.
 */
class Decoder final {
 public:
  explicit Decoder(
      Encoding encoding = Encoding::utf8,
      DecoderOptions options = {});

  /**
   * Construct from a WHATWG encoding label (ASCII case-insensitive, trimmed).
   * Supported: "utf-8", "utf8", "unicode-1-1-utf-8", "utf-16", "utf-16le",
   * "utf-16be". ("utf-16" maps to utf-16le.)
   */
  [[nodiscard]] static Result<Decoder> create(
      std::string_view label,
      DecoderOptions options = {});

  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) noexcept = default;
  Decoder& operator=(Decoder&&) noexcept = default;
  ~Decoder() = default;

  [[nodiscard]] Encoding encoding_kind() const noexcept { return encoding_; }
  [[nodiscard]] std::string_view encoding() const noexcept;
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] bool ignore_bom() const noexcept { return ignore_bom_; }

  /**
   * Decode bytes to a UTF-8 string.
   *
   * @param input  Next chunk (may be empty).
   * @param stream If true, incomplete trailing sequences are kept in the
   *               internal remainder for the next call. If false, remainder
   *               is flushed and incomplete tails are replacement / fatal.
   */
  [[nodiscard]] Result<std::string> decode(
      std::span<const std::uint8_t> input,
      bool stream = false);

  [[nodiscard]] Result<std::string> decode(
      std::string_view bytes,
      bool stream = false) {
    return decode(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size()),
        stream);
  }

 private:
  [[nodiscard]] Result<std::string> decode_utf8(
      std::span<const std::uint8_t> data,
      bool stream);
  [[nodiscard]] Result<std::string> decode_utf16(
      std::span<const std::uint8_t> data,
      bool stream,
      bool little_endian);

  Encoding encoding_{Encoding::utf8};
  bool fatal_{false};
  bool ignore_bom_{false};
  bool bom_seen_{false};
  std::vector<std::uint8_t> remainder_;
};

/** Normalize / parse a label; nullopt if unsupported. */
[[nodiscard]] std::optional<Encoding> parse_encoding_label(
    std::string_view label);

}  // namespace vacps::text
