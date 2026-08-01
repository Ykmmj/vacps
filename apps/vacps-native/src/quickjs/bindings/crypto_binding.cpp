#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "crypto/crypto.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <quickjs.h>

namespace vacps::js {
namespace {

JSValue js_crypto_random_bytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.randomBytes(n)");
  auto n = converter<std::int32_t>::from_js(ctx, argv[0]);
  if (!n || *n < 0) {
    return JS_ThrowTypeError(ctx, "crypto.randomBytes: invalid n");
  }
  auto bytes = vacps::crypto::random_bytes(static_cast<std::size_t>(*n));
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_sha256(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  auto dig = vacps::crypto::sha256(*bytes);
  return bytes_to_js(ctx, dig).release();
}

JSValue js_crypto_sha256_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256Hex(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  auto dig = vacps::crypto::sha256(*bytes);
  return converter<std::string>::to_js(ctx, vacps::crypto::to_hex(dig)).release();
}

JSValue js_crypto_to_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.toHex(bytes)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::to_hex(*bytes)).release();
}

JSValue js_crypto_from_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.fromHex(hex)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::from_hex(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_base64_encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64Encode(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::base64_encode(*bytes)).release();
}

JSValue js_crypto_base64_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64Decode(s)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::base64_decode(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_base64url_encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64UrlEncode(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::base64url_encode(*bytes)).release();
}

JSValue js_crypto_base64url_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64UrlDecode(s)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::base64url_decode(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_ed25519_public_from_private(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519PublicFromPrivate(privateKey)");
  }
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto pub = vacps::crypto::ed25519_public_from_private(*key);
  if (!pub) return throw_error(ctx, pub.error());
  return bytes_to_js(ctx, *pub).release();
}

/**
 * crypto.ed25519SeedFromPrivateKey(base64url) → ArrayBuffer(32)
 * Accepts raw seed or PKCS#8 DER (base64url), OpenSSL-parsed.
 */
JSValue js_crypto_ed25519_seed_from_private_key(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519SeedFromPrivateKey(base64url)");
  }
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto seed = vacps::crypto::ed25519_seed_from_private_key_base64url(*s);
  if (!seed) return throw_error(ctx, seed.error());
  return bytes_to_js(ctx, *seed).release();
}

JSValue js_crypto_ed25519_sign(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_ThrowTypeError(ctx, "crypto.ed25519Sign(privateKey, message)");
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto msg = bytes_from_js(ctx, argv[1]);
  if (!msg) return throw_error(ctx, msg.error());
  auto sig = vacps::crypto::ed25519_sign(
      *key, std::string_view(reinterpret_cast<const char*>(msg->data()), msg->size()));
  if (!sig) return throw_error(ctx, sig.error());
  return bytes_to_js(ctx, *sig).release();
}

JSValue js_crypto_ed25519_verify(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519Verify(publicKey, message, signature)");
  }
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto msg = bytes_from_js(ctx, argv[1]);
  if (!msg) return throw_error(ctx, msg.error());
  auto sig = bytes_from_js(ctx, argv[2]);
  if (!sig) return throw_error(ctx, sig.error());
  auto ok = vacps::crypto::ed25519_verify(
      *key,
      std::string_view(reinterpret_cast<const char*>(msg->data()), msg->size()),
      *sig);
  if (!ok) return throw_error(ctx, ok.error());
  return JS_NewBool(ctx, *ok ? 1 : 0);
}

const JSCFunctionListEntry k_crypto_exports[] = {
    JS_CFUNC_DEF("randomBytes", 1, js_crypto_random_bytes),
    JS_CFUNC_DEF("sha256", 1, js_crypto_sha256),
    JS_CFUNC_DEF("sha256Hex", 1, js_crypto_sha256_hex),
    JS_CFUNC_DEF("toHex", 1, js_crypto_to_hex),
    JS_CFUNC_DEF("fromHex", 1, js_crypto_from_hex),
    JS_CFUNC_DEF("base64Encode", 1, js_crypto_base64_encode),
    JS_CFUNC_DEF("base64Decode", 1, js_crypto_base64_decode),
    JS_CFUNC_DEF("base64UrlEncode", 1, js_crypto_base64url_encode),
    JS_CFUNC_DEF("base64UrlDecode", 1, js_crypto_base64url_decode),
    JS_CFUNC_DEF("ed25519PublicFromPrivate", 1, js_crypto_ed25519_public_from_private),
    JS_CFUNC_DEF("ed25519SeedFromPrivateKey", 1, js_crypto_ed25519_seed_from_private_key),
    JS_CFUNC_DEF("ed25519Sign", 2, js_crypto_ed25519_sign),
    JS_CFUNC_DEF("ed25519Verify", 3, js_crypto_ed25519_verify),
};

int js_crypto_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_crypto_exports, VACPS_COUNTOF(k_crypto_exports));
}


}  // namespace

JSModuleDef* init_module_crypto(JSContext* ctx, const char* name, void* binding) {
  // Pure module: binding is intentionally nullptr (stateless crypto helpers).
  (void)binding;
  JSModuleDef* m = JS_NewCModule(ctx, name, js_crypto_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_crypto_exports, VACPS_COUNTOF(k_crypto_exports));
  return m;
}



}  // namespace vacps::js
