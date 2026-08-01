#include "text/decoder.hpp"

#include <simdutf.h>

#include <cstring>
#include <optional>

namespace vacps::text {
namespace {

std::string ascii_lower_trim(std::string_view in) {
  std::size_t begin = 0;
  std::size_t end = in.size();
  while (begin < end &&
         (in[begin] == ' ' || in[begin] == '\t' || in[begin] == '\n' ||
          in[begin] == '\r' || in[begin] == '\f')) {
    ++begin;
  }
  while (end > begin &&
         (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\n' ||
          in[end - 1] == '\r' || in[end - 1] == '\f')) {
    --end;
  }
  std::string out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    out.push_back(
        static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}

/**
 * Walk UTF-8 with simdutf; fatal → error, else U+FFFD replacement.
 * Uses convert_utf8_to_utf16le_with_errors + convert_utf16le_to_utf8
 * (no hand-rolled UTF-8 state machine).
 */
Result<std::string> utf8_to_well_formed(
    const char* data,
    std::size_t len,
    bool fatal) {
  if (len == 0) {
    return std::string{};
  }
  if (simdutf::validate_utf8(data, len)) {
    return std::string{data, len};
  }
  if (fatal) {
    return std::unexpected(Error{"The encoded data was not valid"});
  }

  std::vector<char16_t> utf16;
  utf16.reserve(len);
  std::size_t pos = 0;
  while (pos < len) {
    const char* chunk = data + pos;
    const std::size_t remain = len - pos;
    const std::size_t budget = simdutf::utf16_length_from_utf8(chunk, remain);
    std::vector<char16_t> tmp(budget == 0 ? 1 : budget);
    const simdutf::result res =
        simdutf::convert_utf8_to_utf16le_with_errors(chunk, remain, tmp.data());
    if (res.error == simdutf::error_code::SUCCESS) {
      utf16.insert(utf16.end(), tmp.data(), tmp.data() + res.count);
      break;
    }
    // res.count = index of first invalid byte (or start of truncated seq).
    const std::size_t err_at = res.count;
    if (err_at > 0) {
      const std::size_t written =
          simdutf::convert_utf8_to_utf16le(chunk, err_at, tmp.data());
      utf16.insert(utf16.end(), tmp.data(), tmp.data() + written);
      pos += err_at;
    }
    // Skip one bad byte (or incomplete lead) and insert U+FFFD.
    utf16.push_back(u'\uFFFD');
    if (pos < len) {
      ++pos;
    } else {
      break;
    }
  }

  const std::size_t out_budget =
      simdutf::utf8_length_from_utf16le(utf16.data(), utf16.size());
  std::string out(out_budget, '\0');
  const std::size_t written =
      simdutf::convert_utf16le_to_utf8(utf16.data(), utf16.size(), out.data());
  out.resize(written);
  return out;
}

/**
 * Stream=true: peel an incomplete trailing UTF-8 sequence into remainder.
 *
 * - When the buffer is valid or only truncated at the end, use
 *   simdutf::trim_partial_utf8 (docs: input assumed valid, possibly truncated).
 * - When invalid mid-buffer, do not call trim_partial on the whole buffer;
 *   locate an incomplete suffix via validate_utf8_with_errors (TOO_SHORT).
 */
void peel_utf8_stream_remainder(
    const char* data,
    std::size_t len,
    std::size_t& keep_len,
    std::vector<std::uint8_t>& remainder) {
  keep_len = len;
  remainder.clear();
  if (len == 0) {
    return;
  }

  const simdutf::result vr = simdutf::validate_utf8_with_errors(data, len);
  if (vr.error == simdutf::error_code::SUCCESS ||
      vr.error == simdutf::error_code::TOO_SHORT) {
    // Prefix is well-formed UTF-8; peel a truncated last character if any.
    keep_len = simdutf::trim_partial_utf8(data, len);
  } else {
    // First error is not a trailing truncation. Still buffer an incomplete
    // suffix when the final 1–3 bytes alone form a TOO_SHORT sequence.
    keep_len = len;
    const std::size_t window = len > 3 ? len - 3 : 0;
    for (std::size_t start = window; start < len; ++start) {
      const simdutf::result tr =
          simdutf::validate_utf8_with_errors(data + start, len - start);
      if (tr.error == simdutf::error_code::TOO_SHORT && tr.count == 0) {
        keep_len = start;
        break;
      }
    }
  }

  if (keep_len < len) {
    remainder.assign(
        reinterpret_cast<const std::uint8_t*>(data + keep_len),
        reinterpret_cast<const std::uint8_t*>(data + len));
  }
}

Result<std::string> utf16_bytes_to_utf8(
    const std::uint8_t* bytes,
    std::size_t byte_len,
    bool little_endian,
    bool fatal) {
  if (byte_len == 0) {
    return std::string{};
  }
  if ((byte_len % 2) != 0) {
    return std::unexpected(Error{"TextDecoder: internal odd UTF-16 length"});
  }
  const std::size_t units = byte_len / 2;
  // Present as char16_t[]; LE/BE APIs interpret unit endianness — no swap loop.
  std::vector<char16_t> buf(units);
  std::memcpy(buf.data(), bytes, byte_len);

  if (little_endian) {
    if (simdutf::validate_utf16le(buf.data(), units)) {
      const std::size_t budget =
          simdutf::utf8_length_from_utf16le(buf.data(), units);
      std::string out(budget, '\0');
      const std::size_t written =
          simdutf::convert_utf16le_to_utf8(buf.data(), units, out.data());
      out.resize(written);
      return out;
    }
    if (fatal) {
      return std::unexpected(Error{"The encoded data was not valid"});
    }
    const auto len_res =
        simdutf::utf8_length_from_utf16le_with_replacement(buf.data(), units);
    std::string out(len_res.count == 0 ? 1 : len_res.count, '\0');
    const std::size_t written =
        simdutf::convert_utf16le_to_utf8_with_replacement(
            buf.data(), units, out.data());
    out.resize(written);
    return out;
  }

  if (simdutf::validate_utf16be(buf.data(), units)) {
    const std::size_t budget =
        simdutf::utf8_length_from_utf16be(buf.data(), units);
    std::string out(budget, '\0');
    const std::size_t written =
        simdutf::convert_utf16be_to_utf8(buf.data(), units, out.data());
    out.resize(written);
    return out;
  }
  if (fatal) {
    return std::unexpected(Error{"The encoded data was not valid"});
  }
  const auto len_res =
      simdutf::utf8_length_from_utf16be_with_replacement(buf.data(), units);
  std::string out(len_res.count == 0 ? 1 : len_res.count, '\0');
  const std::size_t written =
      simdutf::convert_utf16be_to_utf8_with_replacement(
          buf.data(), units, out.data());
  out.resize(written);
  return out;
}

/**
 * Stream=true UTF-16: after aligning to whole code units, peel a trailing
 * unpaired high surrogate with trim_partial_utf16{le,be}.
 *
 * Docs: trim_partial assumes valid-but-possibly-truncated input. Call
 * validate_utf16*_with_errors first; SUCCESS and trailing SURROGATE match
 * that precondition. For mid-buffer errors, trim_partial still only inspects
 * the last unit (lone low surrogates are not incomplete).
 */
void peel_utf16_stream_remainder(
    const std::uint8_t* bytes,
    std::size_t& byte_len,
    bool little_endian,
    std::vector<std::uint8_t>& remainder) {
  if (byte_len < 2) {
    return;
  }
  const std::size_t units = byte_len / 2;
  std::vector<char16_t> buf(units);
  std::memcpy(buf.data(), bytes, units * 2);

  const simdutf::result vr =
      little_endian
          ? simdutf::validate_utf16le_with_errors(buf.data(), units)
          : simdutf::validate_utf16be_with_errors(buf.data(), units);

  const auto trim = [&]() {
    return little_endian ? simdutf::trim_partial_utf16le(buf.data(), units)
                         : simdutf::trim_partial_utf16be(buf.data(), units);
  };

  std::size_t keep_units = units;
  if (vr.error == simdutf::error_code::SUCCESS) {
    keep_units = trim();
  } else if (vr.error == simdutf::error_code::SURROGATE) {
    // Trailing or mid unpaired surrogate — peel high surrogate at end if any.
    keep_units = trim();
  } else {
    // Unexpected UTF-16 validation code; still peel last unit if high.
    keep_units = trim();
  }

  if (keep_units < units) {
    const std::size_t keep_bytes = keep_units * 2;
    remainder.insert(
        remainder.end(), bytes + keep_bytes, bytes + byte_len);
    byte_len = keep_bytes;
  }
}

/**
 * Skip leading BOM per WHATWG TextDecoder (call only when n >= BOM size):
 * - ignoreBOM=false (default) → consume matching BOM
 * - ignoreBOM=true → keep BOM as U+FEFF character
 * Sets bom_seen on every completed check (match or not).
 */
std::size_t skip_bom_if(
    const std::uint8_t* p,
    std::size_t n,
    simdutf::encoding_type expected,
    bool ignore_bom,
    bool& bom_seen) {
  if (bom_seen) {
    return 0;
  }
  bom_seen = true;
  if (ignore_bom || n == 0) {
    return 0;
  }
  const auto found = simdutf::BOM::check_bom(p, n);
  if (found == expected) {
    return simdutf::BOM::bom_byte_size(found);
  }
  return 0;
}

/** Bytes required to decide presence of a BOM for this encoding. */
std::size_t bom_need_bytes(simdutf::encoding_type expected) {
  return simdutf::BOM::bom_byte_size(expected);
}

/** U+FFFD as UTF-8 via simdutf (no hardcoded multi-byte sequence). */
void append_replacement(std::string& out) {
  const char16_t rep = u'\uFFFD';
  char rep_utf8[4]{};
  const std::size_t n = simdutf::convert_utf16le_to_utf8(&rep, 1, rep_utf8);
  out.append(rep_utf8, n);
}

}  // namespace

std::optional<Encoding> parse_encoding_label(std::string_view label) {
  const std::string lower = ascii_lower_trim(label);
  if (lower == "utf-8" || lower == "utf8" || lower == "unicode-1-1-utf-8") {
    return Encoding::utf8;
  }
  if (lower == "utf-16le" || lower == "utf-16") {
    return Encoding::utf16le;
  }
  if (lower == "utf-16be") {
    return Encoding::utf16be;
  }
  return std::nullopt;
}

Decoder::Decoder(Encoding encoding, DecoderOptions options)
    : encoding_(encoding),
      fatal_(options.fatal),
      ignore_bom_(options.ignore_bom) {}

Result<Decoder> Decoder::create(
    std::string_view label,
    DecoderOptions options) {
  auto enc = parse_encoding_label(label);
  if (!enc) {
    return std::unexpected(Error{"TextDecoder: unsupported encoding label"});
  }
  return Decoder{*enc, options};
}

std::string_view Decoder::encoding() const noexcept {
  switch (encoding_) {
    case Encoding::utf8:
      return "utf-8";
    case Encoding::utf16le:
      return "utf-16le";
    case Encoding::utf16be:
      return "utf-16be";
  }
  return "utf-8";
}

Result<std::string> Decoder::decode(
    std::span<const std::uint8_t> input,
    bool stream) {
  std::vector<std::uint8_t> buf;
  buf.reserve(remainder_.size() + input.size());
  buf.insert(buf.end(), remainder_.begin(), remainder_.end());
  buf.insert(buf.end(), input.begin(), input.end());
  remainder_.clear();

  switch (encoding_) {
    case Encoding::utf8:
      return decode_utf8(buf, stream);
    case Encoding::utf16le:
      return decode_utf16(buf, stream, /*little_endian=*/true);
    case Encoding::utf16be:
      return decode_utf16(buf, stream, /*little_endian=*/false);
  }
  return std::unexpected(Error{"TextDecoder: unknown encoding"});
}

Result<std::string> Decoder::decode_utf8(
    std::span<const std::uint8_t> data,
    bool stream) {
  // Streaming BOM: do not set bom_seen until enough bytes for a full check.
  if (!bom_seen_) {
    const std::size_t need = bom_need_bytes(simdutf::encoding_type::UTF8);
    if (data.size() < need) {
      if (stream) {
        // Hold entire chunk; a later chunk may complete EF BB BF.
        remainder_.assign(data.begin(), data.end());
        return std::string{};
      }
      // Final flush: no room for a 3-byte BOM — decode as content.
      bom_seen_ = true;
    }
  }

  const std::size_t start = skip_bom_if(
      data.data(),
      data.size(),
      simdutf::encoding_type::UTF8,
      ignore_bom_,
      bom_seen_);

  const char* p = reinterpret_cast<const char*>(data.data() + start);
  std::size_t len = data.size() - start;

  if (stream && len > 0) {
    std::size_t keep = len;
    peel_utf8_stream_remainder(p, len, keep, remainder_);
    len = keep;
  }

  return utf8_to_well_formed(p, len, fatal_);
}

Result<std::string> Decoder::decode_utf16(
    std::span<const std::uint8_t> data,
    bool stream,
    bool little_endian) {
  const auto expected_bom = little_endian ? simdutf::encoding_type::UTF16_LE
                                          : simdutf::encoding_type::UTF16_BE;

  if (!bom_seen_) {
    const std::size_t need = bom_need_bytes(expected_bom);
    if (data.size() < need) {
      if (stream) {
        remainder_.assign(data.begin(), data.end());
        return std::string{};
      }
      bom_seen_ = true;
    }
  }

  const std::size_t start = skip_bom_if(
      data.data(),
      data.size(),
      expected_bom,
      ignore_bom_,
      bom_seen_);

  const std::uint8_t* p = data.data() + start;
  std::size_t len = data.size() - start;

  // Odd trailing byte cannot form a UTF-16 code unit. Hold it aside so a
  // peeled high-surrogate remainder (if any) stays in byte order before it.
  bool odd_flush = false;
  std::uint8_t odd_byte = 0;
  bool have_odd = false;
  if ((len % 2) == 1) {
    have_odd = true;
    odd_byte = p[len - 1];
    --len;
    if (!stream) {
      odd_flush = true;
    }
  }

  if (odd_flush && fatal_) {
    return std::unexpected(Error{"The encoded data was not valid"});
  }

  // Streaming unpaired high surrogate at end via trim_partial_utf16*.
  if (stream && len >= 2) {
    peel_utf16_stream_remainder(p, len, little_endian, remainder_);
  }

  // Append the incomplete trailing byte after any peeled code unit.
  if (stream && have_odd) {
    remainder_.push_back(odd_byte);
  }

  auto out = utf16_bytes_to_utf8(p, len, little_endian, fatal_);
  if (!out) {
    return out;
  }
  if (odd_flush && !fatal_) {
    append_replacement(*out);
  }
  return out;
}

}  // namespace vacps::text
