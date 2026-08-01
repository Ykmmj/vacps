#include "text/decoder.hpp"
#include "text/encoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

/** UTF-8 BOM EF BB BF */
const std::uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};

std::vector<std::uint8_t> with_utf8_bom(std::string_view body) {
  std::vector<std::uint8_t> out(kUtf8Bom, kUtf8Bom + 3);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace

TEST(TextDecoderTest, IgnoreBomFalseConsumesLeadingBom) {
  // WHATWG default: ignoreBOM=false → strip BOM, do not emit U+FEFF.
  vacps::text::DecoderOptions opts;
  opts.ignore_bom = false;
  auto d = vacps::text::Decoder::create("utf-8", opts);
  ASSERT_TRUE(d) << d.error().message;

  const auto bytes = with_utf8_bom("hello");
  auto out = d->decode(bytes, /*stream=*/false);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(*out, "hello");
  EXPECT_EQ(out->find("\xEF\xBB\xBF"), std::string::npos);
}

TEST(TextDecoderTest, IgnoreBomTrueKeepsBomAsCharacter) {
  // ignoreBOM=true → BOM is decoded as U+FEFF (UTF-8 EF BB BF).
  vacps::text::DecoderOptions opts;
  opts.ignore_bom = true;
  auto d = vacps::text::Decoder::create("utf-8", opts);
  ASSERT_TRUE(d) << d.error().message;

  const auto bytes = with_utf8_bom("hello");
  auto out = d->decode(bytes, /*stream=*/false);
  ASSERT_TRUE(out) << out.error().message;
  // U+FEFF as UTF-8 is the BOM bytes themselves.
  ASSERT_GE(out->size(), 3u);
  EXPECT_EQ(static_cast<unsigned char>((*out)[0]), 0xEF);
  EXPECT_EQ(static_cast<unsigned char>((*out)[1]), 0xBB);
  EXPECT_EQ(static_cast<unsigned char>((*out)[2]), 0xBF);
  EXPECT_EQ(out->substr(3), "hello");
}

TEST(TextDecoderTest, StreamingBomNotSeenUntilEnoughBytes) {
  // Split BOM across stream chunks: do not mark bom_seen until 3 bytes available.
  vacps::text::DecoderOptions opts;
  opts.ignore_bom = false;
  auto d = vacps::text::Decoder::create("utf-8", opts);
  ASSERT_TRUE(d) << d.error().message;

  std::vector<std::uint8_t> part1 = {0xEF, 0xBB};
  auto mid = d->decode(part1, /*stream=*/true);
  ASSERT_TRUE(mid) << mid.error().message;
  EXPECT_TRUE(mid->empty()) << "partial BOM must not emit yet: " << *mid;

  std::vector<std::uint8_t> part2 = {0xBF, 'o', 'k'};
  auto end = d->decode(part2, /*stream=*/false);
  ASSERT_TRUE(end) << end.error().message;
  EXPECT_EQ(*end, "ok");
}

TEST(TextDecoderTest, StreamingBomKeepWhenIgnoreBomTrue) {
  vacps::text::DecoderOptions opts;
  opts.ignore_bom = true;
  auto d = vacps::text::Decoder::create("utf-8", opts);
  ASSERT_TRUE(d) << d.error().message;

  std::vector<std::uint8_t> part1 = {0xEF};
  auto a = d->decode(part1, /*stream=*/true);
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->empty());

  std::vector<std::uint8_t> part2 = {0xBB, 0xBF, 'x'};
  auto b = d->decode(part2, /*stream=*/false);
  ASSERT_TRUE(b) << b.error().message;
  ASSERT_GE(b->size(), 4u);
  EXPECT_EQ(static_cast<unsigned char>((*b)[0]), 0xEF);
  EXPECT_EQ(b->substr(3), "x");
}

TEST(TextEncoderTest, EncodeIntoDoesNotSplitCodeUnits) {
  // Never write half a multi-byte seq; WHATWG {read, written}.
  const std::string_view utf8 = "é";  // C3 A9 — one UTF-16 code unit
  std::uint8_t dest[1] = {0xFF};
  auto r = vacps::text::Encoder::encode_into(
      utf8, std::span<std::uint8_t>(dest, 1));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->written, 0u) << "1-byte dest cannot hold 2-byte code point";
  EXPECT_EQ(r->read, 0u);
  EXPECT_EQ(dest[0], 0xFF) << "dest must be left untouched";

  std::uint8_t dest2[2] = {};
  auto r2 = vacps::text::Encoder::encode_into(
      utf8, std::span<std::uint8_t>(dest2, 2));
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->written, 2u);
  EXPECT_EQ(r2->read, 1u);
  EXPECT_EQ(dest2[0], 0xC3);
  EXPECT_EQ(dest2[1], 0xA9);
}

TEST(TextEncoderTest, EncodeIntoPartialAsciiPrefix) {
  // "aé" into 2-byte dest: only 'a' fits (é needs 2 more bytes).
  const std::string_view utf8 = "aé";  // 61 C3 A9
  std::uint8_t dest[2] = {0xFF, 0xFF};
  auto r = vacps::text::Encoder::encode_into(
      utf8, std::span<std::uint8_t>(dest, 2));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->written, 1u);
  EXPECT_EQ(r->read, 1u);
  EXPECT_EQ(dest[0], static_cast<std::uint8_t>('a'));
  EXPECT_EQ(dest[1], 0xFF) << "second byte must not receive partial UTF-8";
}

TEST(TextEncoderTest, EncodeIntoEmojiSurrogateRead) {
  // U+1F600 😀 → F0 9F 98 80 (4 UTF-8 bytes, 2 UTF-16 code units).
  const char emoji[] = "\xF0\x9F\x98\x80";
  const std::string_view utf8{emoji, 4};
  std::uint8_t dest[4] = {};
  auto r = vacps::text::Encoder::encode_into(
      utf8, std::span<std::uint8_t>(dest, 4));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->written, 4u);
  EXPECT_EQ(r->read, 2u);
}
