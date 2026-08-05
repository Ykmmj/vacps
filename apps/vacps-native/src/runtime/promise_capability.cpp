#include "runtime/promise_capability.hpp"

#include "qjs/scoped_cstring.hpp"
#include "runtime/js_error.hpp"

namespace vacps::runtime {

namespace {

void clear_pending_exception(JSContext* ctx) noexcept {
  JSValue ex = JS_GetException(ctx);
  JS_FreeValue(ctx, ex);
}

}  // namespace

VoidResult PromiseCapability::call_once(
    JSValueConst function,
    JSValueConst argument) noexcept {
  // Narrow: exactly-once settlement while the capability is live. Double
  // settle / moved-from use is programmer error — not product recovery.
  // Native OOM (std::bad_alloc) is fail-fast via this noexcept boundary;
  // QuickJS failures remain operational Result errors.
  settled_ = true;
  JSValueConst argv[] = {argument};
  vacps::qjs::OwnedValue result{
      ctx_, JS_Call(ctx_, function, JS_UNDEFINED, 1, argv)};
  // Settlement consumed the pair; drop handles now.
  resolve_.reset();
  reject_.reset();
  if (result.is_exception()) {
    vacps::qjs::OwnedValue exception{ctx_, JS_GetException(ctx_)};
    auto text =
        vacps::qjs::ScopedCString::from_value(ctx_, exception.get());
    // Preserve prior policy: do not clear a secondary ToCString exception.
    std::string message =
        !text.empty() ? text.str() : "Promise settlement failed";
    return std::unexpected(Error::js(std::move(message)));
  }
  return {};
}

VoidResult PromiseCapability::resolve(JSValueConst value) noexcept {
  return call_once(resolve_.get(), value);
}

VoidResult PromiseCapability::resolve_undefined() noexcept {
  return call_once(resolve_.get(), JS_UNDEFINED);
}

VoidResult PromiseCapability::reject(JSValueConst reason) noexcept {
  return call_once(reject_.get(), reason);
}

VoidResult PromiseCapability::reject_error(const Error& error) noexcept {
  // Native OOM is fail-fast via this noexcept boundary (no allocation Result).
  // QuickJS C API failures remain operational Result errors.
  auto error_value = make_js_error_object(ctx_, error);
  if (!error_value) {
    // make_js_error_object clears its own pending exceptions on failure.
    // Clear again so the string fallback never runs atop a pending exception.
    clear_pending_exception(ctx_);

    JSValue s =
        JS_NewStringLen(ctx_, error.message.data(), error.message.size());
    if (JS_IsException(s)) {
      // Even the fallback string failed — clear and fail closed without
      // calling reject on top of a pending QuickJS exception.
      clear_pending_exception(ctx_);
      return std::unexpected(
          Error::js("failed to build Promise rejection value"));
    }
    auto r = reject(s);
    JS_FreeValue(ctx_, s);
    return r;
  }
  return reject(error_value->get());
}

}  // namespace vacps::runtime
