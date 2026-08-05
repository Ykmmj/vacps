#include "modules/bindings.hpp"

#include "binding/function.hpp"
#include "binding/module.hpp"
#include "crypto/crypto.hpp"
#include "modules/catalog.hpp"

#include <quickjs.h>

#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

using Bytes = std::vector<std::uint8_t>;

/** View binary payload as string_view for crypto APIs that take message bytes. */
[[nodiscard]] std::string_view as_sv(const Bytes& b) noexcept {
  if (b.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/** Named ES exports — declare at module create, set during init. */
constexpr const char* k_crypto_exports[] = {
    "randomBytes",
    "sha256",
    "sha256Hex",
    "toHex",
    "fromHex",
    "base64Encode",
    "base64Decode",
    "base64UrlEncode",
    "base64UrlDecode",
    "ed25519PublicFromPrivate",
    "ed25519SeedFromPrivateKey",
    "ed25519Sign",
    "ed25519Verify",
};

/**
 * Module init (phase 2): create function values and JS_SetModuleExport each name.
 * No C++ exception may escape this C callback.
 */
int initialize_crypto(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    // Pure synchronous module: Env from live JSContext only.
    binding::Env env{ctx};
    binding::ModuleBuilder mod{env};

    // Helper: create_function + set_export with exact ownership transfer.
    auto export_fn = [&](const char* name, auto fn, int length) -> bool {
      qjs::OwnedValue func =
          binding::create_function(env, name, std::move(fn), length);
      return mod.set_export(m, name, std::move(func)) == 0;
    };

    if (!export_fn(
            "randomBytes",
            [](std::int32_t n) -> binding::Result<Bytes> {
              // Public API accepts signed int32 size (not size_t).
              if (n < 0) {
                return std::unexpected(
                    binding::Error::range("size must be non-negative"));
              }
              auto r =
                  vacps::crypto::random_bytes(static_cast<std::size_t>(n));
              if (!r) {
                return std::unexpected(binding::Error::from_domain(r.error()));
              }
              return std::move(*r);
            },
            1)) {
      return -1;
    }
    if (!export_fn(
            "sha256",
            [](Bytes data) -> binding::Result<Bytes> {
              auto digest = vacps::crypto::sha256(data);
              if (!digest) {
                return std::unexpected(
                    binding::Error::from_domain(digest.error()));
              }
              // One heap vector required by the existing ArrayBuffer converter.
              return Bytes(digest->begin(), digest->end());
            },
            1)) {
      return -1;
    }
    if (!export_fn(
            "sha256Hex",
            [](Bytes data) -> binding::Result<std::string> {
              auto digest = vacps::crypto::sha256(data);
              if (!digest) {
                return std::unexpected(
                    binding::Error::from_domain(digest.error()));
              }
              // Hex-encode the fixed digest directly (no intermediate vector).
              return vacps::crypto::to_hex(*digest);
            },
            1)) {
      return -1;
    }
    if (!export_fn(
            "toHex",
            [](Bytes data) { return vacps::crypto::to_hex(data); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "fromHex",
            [](std::string hex) { return vacps::crypto::from_hex(hex); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "base64Encode",
            [](Bytes data) { return vacps::crypto::base64_encode(data); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "base64Decode",
            [](std::string s) { return vacps::crypto::base64_decode(s); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "base64UrlEncode",
            [](Bytes data) { return vacps::crypto::base64url_encode(data); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "base64UrlDecode",
            [](std::string s) { return vacps::crypto::base64url_decode(s); },
            1)) {
      return -1;
    }
    if (!export_fn(
            "ed25519PublicFromPrivate",
            [](Bytes key) {
              return vacps::crypto::ed25519_public_from_private(key);
            },
            1)) {
      return -1;
    }
    if (!export_fn(
            "ed25519SeedFromPrivateKey",
            [](std::string encoded) {
              return vacps::crypto::ed25519_seed_from_private_key_base64url(
                  encoded);
            },
            1)) {
      return -1;
    }
    if (!export_fn(
            "ed25519Sign",
            [](Bytes key, Bytes msg) {
              return vacps::crypto::ed25519_sign(key, as_sv(msg));
            },
            2)) {
      return -1;
    }
    if (!export_fn(
            "ed25519Verify",
            [](Bytes key, Bytes msg, Bytes sig) {
              return vacps::crypto::ed25519_verify(key, as_sv(msg), sig);
            },
            3)) {
      return -1;
    }

    return 0;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return -1;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return -1;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "crypto module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_crypto(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_crypto);
    if (m == nullptr) {
      return nullptr;
    }

    // Phase 1: declare every named export (required before SetModuleExport).
    for (const char* export_name : k_crypto_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "crypto module: declare_export failed");
        }
        return nullptr;
      }
    }
    return m;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return nullptr;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return nullptr;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "crypto module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
