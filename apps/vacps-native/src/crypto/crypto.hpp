#pragma once

#include "app/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::crypto {

/** Fixed 32-byte SHA-256 digest value (no heap). */
using Sha256Digest = std::array<std::uint8_t, 32>;

/** Cryptographically secure random (OpenSSL RAND_bytes). */
[[nodiscard]] Result<std::vector<std::uint8_t>> random_bytes(std::size_t n);

// Contract: Wide
// Preconditions: none beyond type-representable arguments (any byte sequence)
// Errors: Result error on OpenSSL digest failure (operational)
// Threading: any
// Lifetime: caller owns returned digest; input viewed only for the call
/** One-shot SHA-256 over `data` (OpenSSL EVP_Digest + EVP_sha256). */
[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::uint8_t> data);

// Contract: Wide
// Preconditions: none beyond type-representable arguments
// Errors: none
// Threading: any
// Lifetime: caller owns returned string; input viewed only for the call
/** Lowercase hex encoding of arbitrary bytes (no intermediate vector required). */
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

[[nodiscard]] Result<std::vector<std::uint8_t>> from_hex(std::string_view hex);

/**
 * Standard Base64 (OpenSSL EVP_EncodeBlock / EVP_DecodeBlock).
 * Encode: with `=` padding, no newlines.
 * Decode: accepts whitespace-free standard alphabet; padding optional if length % 4 ok after pad.
 */
[[nodiscard]] std::string base64_encode(const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::string base64_encode(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::string base64_encode(std::string_view data);
[[nodiscard]] Result<std::vector<std::uint8_t>> base64_decode(std::string_view b64);

/**
 * Base64url (RFC 4648 §5): `-_` instead of `+/`, no padding on encode.
 * Decode accepts unpadded or padded input.
 */
[[nodiscard]] std::string base64url_encode(const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::string base64url_encode(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::string base64url_encode(std::string_view data);
[[nodiscard]] Result<std::vector<std::uint8_t>> base64url_decode(std::string_view b64url);

/**
 * Ed25519 via OpenSSL 3 raw keys.
 * private_key_32 / public_key_32: 32-byte seed / public key.
 * signature: 64 bytes.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> ed25519_sign(
    const std::vector<std::uint8_t>& private_key_32,
    std::string_view message);

[[nodiscard]] Result<bool> ed25519_verify(
    const std::vector<std::uint8_t>& public_key_32,
    std::string_view message,
    const std::vector<std::uint8_t>& signature_64);

/** Derive 32-byte public key from 32-byte Ed25519 private seed. */
[[nodiscard]] Result<std::vector<std::uint8_t>> ed25519_public_from_private(
    const std::vector<std::uint8_t>& private_key_32);

/**
 * Parse AGENT_PRIVATE_KEY material (base64url):
 * - raw 32-byte seed, or
 * - PKCS#8 / DER private key (OpenSSL d2i_AutoPrivateKey + get_raw_private_key).
 * Returns 32-byte seed for ed25519_sign / ed25519_public_from_private.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> ed25519_seed_from_private_key_base64url(
    std::string_view encoded);

}  // namespace vacps::crypto
