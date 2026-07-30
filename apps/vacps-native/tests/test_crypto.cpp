#include "crypto/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/crypto.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(CryptoTest, Sha256Abc) {
  const auto dig = vacps::crypto::sha256(std::string_view{"abc"});
  ASSERT_EQ(dig.size(), 32u);
  EXPECT_EQ(
      vacps::crypto::to_hex(dig),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(CryptoTest, Sha256Empty) {
  const auto dig = vacps::crypto::sha256(std::string_view{""});
  EXPECT_EQ(
      vacps::crypto::to_hex(dig),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(CryptoTest, HexRoundTrip) {
  const auto bytes = vacps::crypto::sha256(std::string_view{"vacps"});
  const auto hex = vacps::crypto::to_hex(bytes);
  auto back = vacps::crypto::from_hex(hex);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, bytes);
}

TEST(CryptoTest, FromHexRejectsOddLength) {
  auto r = vacps::crypto::from_hex("abc");
  EXPECT_FALSE(r);
}

TEST(CryptoTest, RandomBytesLength) {
  auto a = vacps::crypto::random_bytes(16);
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_EQ(a->size(), 16u);

  auto b = vacps::crypto::random_bytes(16);
  ASSERT_TRUE(b) << b.error().message;
  EXPECT_NE(*a, *b);
}

TEST(CryptoTest, Ed25519SignVerifyRoundTrip) {
  auto seed = vacps::crypto::random_bytes(32);
  ASSERT_TRUE(seed) << seed.error().message;

  auto pub = vacps::crypto::ed25519_public_from_private(*seed);
  ASSERT_TRUE(pub) << pub.error().message;
  ASSERT_EQ(pub->size(), 32u);

  constexpr std::string_view msg = "vacps-ed25519";
  auto sig = vacps::crypto::ed25519_sign(*seed, msg);
  ASSERT_TRUE(sig) << sig.error().message;
  EXPECT_EQ(sig->size(), 64u);

  auto ok = vacps::crypto::ed25519_verify(*pub, msg, *sig);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_TRUE(*ok);

  auto bad = vacps::crypto::ed25519_verify(*pub, "tampered", *sig);
  ASSERT_TRUE(bad) << bad.error().message;
  EXPECT_FALSE(*bad);
}

TEST(CryptoTest, Ed25519RejectsBadKeySize) {
  std::vector<std::uint8_t> short_key(16, 1);
  auto sig = vacps::crypto::ed25519_sign(short_key, "x");
  EXPECT_FALSE(sig);
}

TEST(CryptoTest, Base64RoundTrip) {
  constexpr std::string_view msg = "vacps-base64";
  const auto enc = vacps::crypto::base64_encode(msg);
  EXPECT_EQ(enc, "dmFjcHMtYmFzZTY0");
  auto dec = vacps::crypto::base64_decode(enc);
  ASSERT_TRUE(dec) << dec.error().message;
  EXPECT_EQ(std::string(dec->begin(), dec->end()), msg);
}

TEST(CryptoTest, Base64Empty) {
  EXPECT_TRUE(vacps::crypto::base64_encode(std::string_view{}).empty());
  auto dec = vacps::crypto::base64_decode("");
  ASSERT_TRUE(dec) << dec.error().message;
  EXPECT_TRUE(dec->empty());
}

TEST(CryptoTest, Base64UrlRoundTripNoPadding) {
  // 1 byte → standard "YQ==", url "YQ"
  const auto enc = vacps::crypto::base64url_encode(std::string_view{"a"});
  EXPECT_EQ(enc, "YQ");
  auto dec = vacps::crypto::base64url_decode(enc);
  ASSERT_TRUE(dec) << dec.error().message;
  ASSERT_EQ(dec->size(), 1u);
  EXPECT_EQ((*dec)[0], 'a');
}

TEST(CryptoTest, Base64UrlRejectsInvalid) {
  auto bad = vacps::crypto::base64url_decode("!!!not-valid!!!");
  EXPECT_FALSE(bad);
}

TEST(CryptoTest, Ed25519SeedFromRawBase64Url) {
  auto seed = vacps::crypto::random_bytes(32);
  ASSERT_TRUE(seed) << seed.error().message;
  const auto b64 = vacps::crypto::base64url_encode(*seed);
  auto back = vacps::crypto::ed25519_seed_from_private_key_base64url(b64);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, *seed);
}

TEST(CryptoTest, Ed25519SeedFromPkcs8Base64Url) {
  auto seed = vacps::crypto::random_bytes(32);
  ASSERT_TRUE(seed) << seed.error().message;

  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_ED25519, nullptr, seed->data(), seed->size());
  ASSERT_NE(pkey, nullptr);
  unsigned char* der = nullptr;
  const int n = i2d_PrivateKey(pkey, &der);
  EVP_PKEY_free(pkey);
  ASSERT_GT(n, 0);
  ASSERT_NE(der, nullptr);
  std::vector<std::uint8_t> der_bytes(der, der + n);
  OPENSSL_free(der);

  const auto b64 = vacps::crypto::base64url_encode(der_bytes);
  auto back = vacps::crypto::ed25519_seed_from_private_key_base64url(b64);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, *seed);

  // Sign with parsed seed must match original seed
  auto sig1 = vacps::crypto::ed25519_sign(*seed, "roundtrip");
  auto sig2 = vacps::crypto::ed25519_sign(*back, "roundtrip");
  ASSERT_TRUE(sig1);
  ASSERT_TRUE(sig2);
  EXPECT_EQ(*sig1, *sig2);
}
