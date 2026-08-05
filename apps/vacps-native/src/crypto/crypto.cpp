#include "crypto/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
// d2i_AutoPrivateKey for PKCS#8 / traditional DER private keys

#include <format>

namespace vacps::crypto {
namespace {

struct EvpMdCtx {
  EVP_MD_CTX* p{nullptr};
  EvpMdCtx() : p(EVP_MD_CTX_new()) {}
  ~EvpMdCtx() {
    if (p) EVP_MD_CTX_free(p);
  }
  EvpMdCtx(const EvpMdCtx&) = delete;
  EvpMdCtx& operator=(const EvpMdCtx&) = delete;
};

struct EvpPkey {
  EVP_PKEY* p{nullptr};
  ~EvpPkey() {
    if (p) EVP_PKEY_free(p);
  }
  EvpPkey() = default;
  EvpPkey(const EvpPkey&) = delete;
  EvpPkey& operator=(const EvpPkey&) = delete;
};

}  // namespace

Result<std::vector<std::uint8_t>> random_bytes(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  if (n == 0) {
    return out;
  }
  if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
    return std::unexpected(Error{"OpenSSL RAND_bytes failed"});
  }
  return out;
}

Result<Sha256Digest> sha256(std::span<const std::uint8_t> data) {
  Sha256Digest out{};
  unsigned int out_len = 0;
  // One-shot EVP_Digest: stack digest only; no EVP_MAX_MD_SIZE heap buffer.
  if (EVP_Digest(
          data.data(),
          data.size(),
          out.data(),
          &out_len,
          EVP_sha256(),
          nullptr) != 1 ||
      out_len != static_cast<unsigned int>(out.size())) {
    return std::unexpected(Error{"OpenSSL EVP_Digest(SHA-256) failed"});
  }
  return out;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0xf];
  }
  return out;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return std::unexpected(Error{"hex length must be even"});
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected(Error{"invalid hex character"});
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::string base64_encode(const std::uint8_t* data, std::size_t len) {
  if (len == 0) return {};
  // EVP_EncodeBlock needs 4 * ceil(n/3) + 1 (NUL).
  const std::size_t out_cap = 4 * ((len + 2) / 3) + 1;
  std::string out(out_cap, '\0');
  const int n = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(out.data()),
      data,
      static_cast<int>(len));
  if (n < 0) {
    out.clear();
    return out;
  }
  out.resize(static_cast<std::size_t>(n));
  return out;
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
  return base64_encode(data.data(), data.size());
}

std::string base64_encode(std::string_view data) {
  return base64_encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

Result<std::vector<std::uint8_t>> base64_decode(std::string_view b64) {
  // Strip whitespace; require standard alphabet + padding.
  std::string cleaned;
  cleaned.reserve(b64.size());
  for (char c : b64) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    cleaned.push_back(c);
  }
  if (cleaned.empty()) return std::vector<std::uint8_t>{};

  // Pad to multiple of 4 if missing padding.
  while (cleaned.size() % 4 != 0) cleaned.push_back('=');

  for (char c : cleaned) {
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '+' || c == '/' || c == '=';
    if (!ok) return std::unexpected(Error{"invalid base64 character"});
  }

  // EVP_DecodeBlock writes 3*(n/4) bytes (including padding garbage).
  std::vector<std::uint8_t> out(3 * (cleaned.size() / 4));
  const int n = EVP_DecodeBlock(
      out.data(),
      reinterpret_cast<const unsigned char*>(cleaned.data()),
      static_cast<int>(cleaned.size()));
  if (n < 0) {
    return std::unexpected(Error{"OpenSSL EVP_DecodeBlock failed"});
  }
  // Trim padding bytes indicated by trailing '='.
  std::size_t pad = 0;
  if (!cleaned.empty() && cleaned.back() == '=') {
    pad = 1;
    if (cleaned.size() >= 2 && cleaned[cleaned.size() - 2] == '=') pad = 2;
  }
  const auto real = static_cast<std::size_t>(n) - pad;
  if (real > out.size()) {
    return std::unexpected(Error{"base64 decode length overflow"});
  }
  out.resize(real);
  return out;
}

std::string base64url_encode(const std::uint8_t* data, std::size_t len) {
  std::string s = base64_encode(data, len);
  for (char& c : s) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

std::string base64url_encode(const std::vector<std::uint8_t>& data) {
  return base64url_encode(data.data(), data.size());
}

std::string base64url_encode(std::string_view data) {
  return base64url_encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

Result<std::vector<std::uint8_t>> base64url_decode(std::string_view b64url) {
  std::string std_b64;
  std_b64.reserve(b64url.size() + 3);
  for (char c : b64url) {
    if (c == '-') std_b64.push_back('+');
    else if (c == '_') std_b64.push_back('/');
    else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else std_b64.push_back(c);
  }
  // Reject characters that are neither base64url nor already standard.
  for (char c : std_b64) {
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '+' || c == '/' || c == '=';
    if (!ok) return std::unexpected(Error{"invalid base64url character"});
  }
  return base64_decode(std_b64);
}

Result<std::vector<std::uint8_t>> ed25519_sign(
    const std::vector<std::uint8_t>& private_key_32,
    std::string_view message) {
  if (private_key_32.size() != 32) {
    return std::unexpected(Error{"ed25519_sign: private key must be 32 bytes"});
  }
  EvpPkey pkey;
  pkey.p = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_ED25519, nullptr, private_key_32.data(), private_key_32.size());
  if (pkey.p == nullptr) {
    return std::unexpected(Error{"ed25519_sign: EVP_PKEY_new_raw_private_key failed"});
  }

  EvpMdCtx md;
  if (md.p == nullptr ||
      EVP_DigestSignInit(md.p, nullptr, nullptr, nullptr, pkey.p) != 1) {
    return std::unexpected(Error{"ed25519_sign: DigestSignInit failed"});
  }

  std::size_t sig_len = 0;
  if (EVP_DigestSign(
          md.p,
          nullptr,
          &sig_len,
          reinterpret_cast<const std::uint8_t*>(message.data()),
          message.size()) != 1) {
    return std::unexpected(Error{"ed25519_sign: DigestSign size failed"});
  }
  std::vector<std::uint8_t> sig(sig_len);
  if (EVP_DigestSign(
          md.p,
          sig.data(),
          &sig_len,
          reinterpret_cast<const std::uint8_t*>(message.data()),
          message.size()) != 1) {
    return std::unexpected(Error{"ed25519_sign: DigestSign failed"});
  }
  sig.resize(sig_len);
  return sig;
}

Result<bool> ed25519_verify(
    const std::vector<std::uint8_t>& public_key_32,
    std::string_view message,
    const std::vector<std::uint8_t>& signature_64) {
  if (public_key_32.size() != 32) {
    return std::unexpected(Error{"ed25519_verify: public key must be 32 bytes"});
  }
  if (signature_64.size() != 64) {
    return std::unexpected(Error{"ed25519_verify: signature must be 64 bytes"});
  }
  EvpPkey pkey;
  pkey.p = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr, public_key_32.data(), public_key_32.size());
  if (pkey.p == nullptr) {
    return std::unexpected(Error{"ed25519_verify: EVP_PKEY_new_raw_public_key failed"});
  }

  EvpMdCtx md;
  if (md.p == nullptr ||
      EVP_DigestVerifyInit(md.p, nullptr, nullptr, nullptr, pkey.p) != 1) {
    return std::unexpected(Error{"ed25519_verify: DigestVerifyInit failed"});
  }
  const int ok = EVP_DigestVerify(
      md.p,
      signature_64.data(),
      signature_64.size(),
      reinterpret_cast<const std::uint8_t*>(message.data()),
      message.size());
  if (ok == 1) {
    return true;
  }
  if (ok == 0) {
    return false;  // invalid signature
  }
  return std::unexpected(Error{"ed25519_verify: DigestVerify failed"});
}

Result<std::vector<std::uint8_t>> ed25519_public_from_private(
    const std::vector<std::uint8_t>& private_key_32) {
  if (private_key_32.size() != 32) {
    return std::unexpected(Error{"ed25519_public_from_private: private key must be 32 bytes"});
  }
  EvpPkey pkey;
  pkey.p = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_ED25519, nullptr, private_key_32.data(), private_key_32.size());
  if (pkey.p == nullptr) {
    return std::unexpected(Error{"ed25519_public_from_private: new_raw_private_key failed"});
  }
  std::size_t len = 32;
  std::vector<std::uint8_t> pub(32);
  if (EVP_PKEY_get_raw_public_key(pkey.p, pub.data(), &len) != 1 || len != 32) {
    return std::unexpected(Error{"ed25519_public_from_private: get_raw_public_key failed"});
  }
  return pub;
}

Result<std::vector<std::uint8_t>> ed25519_seed_from_private_key_base64url(
    std::string_view encoded) {
  auto der = base64url_decode(encoded);
  if (!der) {
    return std::unexpected(Error{
        std::format("ed25519 seed: base64url decode failed: {}", der.error().message)});
  }
  if (der->size() == 32) {
    return *der;  // raw seed
  }
  if (der->empty()) {
    return std::unexpected(Error{"ed25519 seed: empty key material"});
  }

  // PKCS#8 or other DER private key → raw seed via OpenSSL.
  const unsigned char* p = der->data();
  long len = static_cast<long>(der->size());
  EvpPkey pkey;
  pkey.p = d2i_AutoPrivateKey(nullptr, &p, len);
  if (pkey.p == nullptr) {
    return std::unexpected(Error{
        "ed25519 seed: not a raw 32-byte seed or parseable PKCS#8/DER private key"});
  }
  if (EVP_PKEY_id(pkey.p) != EVP_PKEY_ED25519) {
    return std::unexpected(Error{"ed25519 seed: private key is not Ed25519"});
  }
  std::size_t seed_len = 32;
  std::vector<std::uint8_t> seed(32);
  if (EVP_PKEY_get_raw_private_key(pkey.p, seed.data(), &seed_len) != 1 || seed_len != 32) {
    return std::unexpected(Error{"ed25519 seed: EVP_PKEY_get_raw_private_key failed"});
  }
  return seed;
}

}  // namespace vacps::crypto
