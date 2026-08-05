#pragma once

/**
 * Env — non-owning JSContext view for the binding DSL.
 *
 * Thin interface for:
 * - creating owned JS values
 * - throwing JS exceptions via binding::error helpers
 * - optional Runtime::Async* (async DSL capability; carried into
 *   NativeSlot; Runtime::Async remains sole Promise/settlement owner)
 *
 * Does not own the context and does not hide QuickJS ownership: every
 * returned qjs::OwnedValue is caller-owned; argv/this remain borrowed elsewhere.
 *
 * Capability notes:
 *   Env always has a caller-supplied live JSContext. Async may be null for
 *   pure synchronous modules/globals.
 *   create_function, create_async_function, and ClassBuilder/ModuleBuilder
 *   copy the pointer into NativeSlot so callbacks rebuild Env{jc, async}.
 *
 *   Synchronous callbacks execute directly in the current owner-thread
 *   QuickJS turn (no per-callback Runtime gate). Async free/method
 *   registration requires a non-null Runtime::Async*; call entry does not
 *   re-check is_accepting. The host wires a non-owning pointer to the
 *   Runtime-owned stable Async capability (Runtime::Impl capabilities)
 *   for the engine lifetime.
 */

#include "binding/error.hpp"
#include "qjs/owned_value.hpp"
#include "runtime/runtime_fwd.hpp"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace vacps::binding {

class Env {
 public:
  /**
   * Binding context with optional Async capability. Null keeps pure
   * synchronous DSL construction viable.
   */
  explicit Env(
      JSContext* ctx,
      Runtime::Async* async = nullptr) noexcept
      : ctx_(ctx), async_(async) {}

  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }
  [[nodiscard]] JSRuntime* runtime() const noexcept {
    return JS_GetRuntime(ctx_);
  }
  /**
   * Optional Runtime::Async capability. Nullptr unless the host constructs
   * Env(ctx, async) with a non-null async. Required at JS call entry for
   * create_async_function / async_method callbacks.
   */
  [[nodiscard]] Runtime::Async* async() const noexcept { return async_; }

  // ── value factories (caller owns returned qjs::OwnedValue) ────────────

  [[nodiscard]] qjs::OwnedValue undefined() const {
    return qjs::OwnedValue::undefined(ctx_);
  }
  [[nodiscard]] qjs::OwnedValue null_value() const {
    return qjs::OwnedValue::nullish(ctx_);
  }
  [[nodiscard]] qjs::OwnedValue boolean(bool v) const {
    return qjs::OwnedValue{ctx_, JS_NewBool(ctx_, v ? 1 : 0)};
  }
  [[nodiscard]] qjs::OwnedValue int32(std::int32_t v) const {
    return qjs::OwnedValue{ctx_, JS_NewInt32(ctx_, v)};
  }
  [[nodiscard]] qjs::OwnedValue int64(std::int64_t v) const {
    return qjs::OwnedValue{ctx_, JS_NewInt64(ctx_, v)};
  }
  [[nodiscard]] qjs::OwnedValue uint32(std::uint32_t v) const {
    return qjs::OwnedValue{ctx_, JS_NewUint32(ctx_, v)};
  }
  [[nodiscard]] qjs::OwnedValue float64(double v) const {
    return qjs::OwnedValue{ctx_, JS_NewFloat64(ctx_, v)};
  }
  [[nodiscard]] qjs::OwnedValue string(std::string_view s) const {
    return qjs::OwnedValue{ctx_, JS_NewStringLen(ctx_, s.data(), s.size())};
  }
  [[nodiscard]] qjs::OwnedValue array_buffer(
      const std::uint8_t* data,
      std::size_t len) const {
    return qjs::OwnedValue{ctx_, JS_NewArrayBufferCopy(ctx_, data, len)};
  }
  [[nodiscard]] qjs::OwnedValue new_object() const {
    return qjs::OwnedValue{ctx_, JS_NewObject(ctx_)};
  }
  [[nodiscard]] qjs::OwnedValue new_array() const {
    return qjs::OwnedValue{ctx_, JS_NewArray(ctx_)};
  }

  // ── exception helpers (return JS_EXCEPTION raw for direct returns) ─

  [[nodiscard]] JSValue throw_type(std::string_view msg) const noexcept {
    return vacps::binding::throw_type(ctx_, msg);
  }
  [[nodiscard]] JSValue throw_range(std::string_view msg) const noexcept {
    return vacps::binding::throw_range(ctx_, msg);
  }
  [[nodiscard]] JSValue throw_internal(std::string_view msg) const noexcept {
    return vacps::binding::throw_internal(ctx_, msg);
  }
  [[nodiscard]] JSValue throw_err(const Error& e) const noexcept {
    return vacps::binding::throw_error(ctx_, e);
  }

 private:
  JSContext* ctx_;
  Runtime::Async* async_;
};

}  // namespace vacps::binding
