#include "runtime/js_error.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>

namespace vacps::runtime {

namespace {

void clear_pending_exception(JSContext* ctx) noexcept {
  JS_FreeValue(ctx, JS_GetException(ctx));
}

[[nodiscard]] bool set_string_prop(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    const char* text) noexcept {
  JSValue v = JS_NewString(ctx, text != nullptr ? text : "");
  if (JS_IsException(v)) {
    return false;
  }
  const int rc = JS_SetPropertyStr(ctx, obj, name, v);
  if (rc < 0) {
    clear_pending_exception(ctx);
    return false;
  }
  return true;
}

[[nodiscard]] bool set_string_prop_len(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    std::string_view text) noexcept {
  JSValue v = JS_NewStringLen(
      ctx, text.data(), static_cast<size_t>(text.size()));
  if (JS_IsException(v)) {
    return false;
  }
  const int rc = JS_SetPropertyStr(ctx, obj, name, v);
  if (rc < 0) {
    clear_pending_exception(ctx);
    return false;
  }
  return true;
}

[[nodiscard]] bool set_int_prop(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    int value) noexcept {
  JSValue v = JS_NewInt32(ctx, value);
  if (JS_IsException(v)) {
    return false;
  }
  const int rc = JS_SetPropertyStr(ctx, obj, name, v);
  if (rc < 0) {
    clear_pending_exception(ctx);
    return false;
  }
  return true;
}

/**
 * Symbolic errno name for JS Error.code when system_code is a POSIX errno.
 * Returns nullptr when no stable name is known.
 */
[[nodiscard]] const char* errno_symbol(int err) noexcept {
  switch (err) {
    case EPERM:
      return "EPERM";
    case ENOENT:
      return "ENOENT";
    case EIO:
      return "EIO";
    case ENXIO:
      return "ENXIO";
    case EBADF:
      return "EBADF";
    case EAGAIN:
      return "EAGAIN";
    case ENOMEM:
      return "ENOMEM";
    case EACCES:
      return "EACCES";
    case EFAULT:
      return "EFAULT";
    case EBUSY:
      return "EBUSY";
    case EEXIST:
      return "EEXIST";
    case EXDEV:
      return "EXDEV";
    case ENODEV:
      return "ENODEV";
    case ENOTDIR:
      return "ENOTDIR";
    case EISDIR:
      return "EISDIR";
    case EINVAL:
      return "EINVAL";
    case ENFILE:
      return "ENFILE";
    case EMFILE:
      return "EMFILE";
    case ENOSPC:
      return "ENOSPC";
    case EROFS:
      return "EROFS";
    case EPIPE:
      return "EPIPE";
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case ERANGE:
      return "ERANGE";
    case ENAMETOOLONG:
      return "ENAMETOOLONG";
    case ENOSYS:
      return "ENOSYS";
    case ENOTEMPTY:
      return "ENOTEMPTY";
    case ELOOP:
      return "ELOOP";
    case EOVERFLOW:
      return "EOVERFLOW";
    case ECANCELED:
      return "ECANCELED";
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:
      return "EOPNOTSUPP";
#endif
    default:
      return nullptr;
  }
}

/** JS Error.code string for a runtime Error. */
[[nodiscard]] const char* error_code_string(const Error& error) noexcept {
  if (error.code == Errc::cancelled) {
    return "ERR_CANCELLED";
  }
  if (error.code == Errc::native_failure && error.system_code != 0) {
    if (const char* sym = errno_symbol(error.system_code); sym != nullptr) {
      return sym;
    }
  }
  return error_code_name(error.code);
}

}  // namespace

const char* error_code_name(Errc code) noexcept {
  switch (code) {
    case Errc::invalid_state:
      return "ERR_INVALID_STATE";
    case Errc::runtime_stopping:
      return "ERR_RUNTIME_STOPPING";
    case Errc::cancelled:
      return "ERR_CANCELLED";
    case Errc::js_exception:
      return "ERR_JS_EXCEPTION";
    case Errc::native_failure:
      return "ERR_NATIVE_FAILURE";
    case Errc::allocation_failure:
      return "ERR_ALLOCATION_FAILURE";
    case Errc::internal_error:
    default:
      return "ERR_INTERNAL";
  }
}

Result<vacps::qjs::OwnedValue> make_js_error_object(
    JSContext* ctx,
    const Error& error) noexcept {
  // Ordinary Error (not TypeError) for native/cancelled/internal failures.
  vacps::qjs::OwnedValue error_value{ctx, JS_NewError(ctx)};
  if (error_value.is_exception()) {
    clear_pending_exception(ctx);
    return std::unexpected(Error::js("JS_NewError failed"));
  }

  if (!set_string_prop_len(ctx, error_value.get(), "message", error.message)) {
    clear_pending_exception(ctx);
    return std::unexpected(Error::js("failed to set Error.message"));
  }

  const char* code = error_code_string(error);
  if (!set_string_prop(ctx, error_value.get(), "code", code)) {
    clear_pending_exception(ctx);
    return std::unexpected(Error::js("failed to set Error.code"));
  }

  if (!error.operation.empty()) {
    if (!set_string_prop_len(
            ctx, error_value.get(), "operation", error.operation)) {
      clear_pending_exception(ctx);
      return std::unexpected(Error::js("failed to set Error.operation"));
    }
  }

  if (error.system_code != 0) {
    // Numeric errno for programmatic checks (alongside symbolic code).
    if (!set_int_prop(ctx, error_value.get(), "errno", error.system_code)) {
      clear_pending_exception(ctx);
      return std::unexpected(Error::js("failed to set Error.errno"));
    }
  }
  return error_value;
}

JSValue throw_js_error(JSContext* ctx, const Error& error) noexcept {
  // Domain-facing validation (invalid_state) → TypeError.
  // Native I/O failures (native_failure / cancelled / …) → ordinary Error.
  if (error.code == Errc::invalid_state) {
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) {
      return global;
    }
    JSValue type_error_ctor = JS_GetPropertyStr(ctx, global, "TypeError");
    JS_FreeValue(ctx, global);
    if (JS_IsException(type_error_ctor) ||
        !JS_IsFunction(ctx, type_error_ctor)) {
      JS_FreeValue(ctx, type_error_ctor);
      clear_pending_exception(ctx);
      const char* msg =
          error.message.empty() ? "TypeError" : error.message.c_str();
      return JS_ThrowTypeError(ctx, "%s", msg);
    }
    const char* msg =
        error.message.empty() ? "TypeError" : error.message.c_str();
    JSValue msg_v = JS_NewString(ctx, msg);
    if (JS_IsException(msg_v)) {
      JS_FreeValue(ctx, type_error_ctor);
      return msg_v;
    }
    JSValueConst args[1] = {msg_v};
    JSValue err = JS_CallConstructor(ctx, type_error_ctor, 1, args);
    JS_FreeValue(ctx, msg_v);
    JS_FreeValue(ctx, type_error_ctor);
    if (JS_IsException(err)) {
      return err;
    }
    const char* code = error_code_string(error);
    if (!set_string_prop(ctx, err, "code", code)) {
      clear_pending_exception(ctx);
    }
    return JS_Throw(ctx, err);
  }

  auto object = make_js_error_object(ctx, error);
  if (!object) {
    clear_pending_exception(ctx);
    const char* msg =
        error.message.empty() ? "native error" : error.message.c_str();
    return JS_ThrowInternalError(ctx, "%s", msg);
  }
  return JS_Throw(ctx, object->release());
}

}  // namespace vacps::runtime
