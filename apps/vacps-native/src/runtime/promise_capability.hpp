#pragma once

/**
 * One JS Promise resolve/reject pair. Never leaves the JS thread.
 *
 * Exactly-once settlement is a Narrow internal invariant. Double settle or
 * use-after-move is programmer error and is not checked here.
 * QuickJS allocation / JS-call failures from the actual settlement attempt
 * remain returned operational errors. Native std::bad_alloc during settlement
 * or rejection construction is fail-fast (terminate via noexcept) — never an
 * allocation Result / Promise rejection payload.
 *
 * The engine and JSContext outlive this capability by Runtime's natural-drain
 * contract: FreeContext runs only after main_io_.run() returns with no
 * outstanding work, so every co_spawn completion handler has already settled
 * and released its capability.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

namespace vacps::runtime {

class PromiseCapability {
 public:
  PromiseCapability(JSContext* ctx, JSValue resolve, JSValue reject) noexcept
      : ctx_(ctx), resolve_(ctx, resolve), reject_(ctx, reject) {}

  PromiseCapability(const PromiseCapability&) = delete;
  PromiseCapability& operator=(const PromiseCapability&) = delete;
  PromiseCapability(PromiseCapability&&) noexcept = default;
  PromiseCapability& operator=(PromiseCapability&&) noexcept = default;

  [[nodiscard]] VoidResult resolve(JSValueConst value) noexcept;
  [[nodiscard]] VoidResult resolve_undefined() noexcept;
  [[nodiscard]] VoidResult reject(JSValueConst reason) noexcept;
  [[nodiscard]] VoidResult reject_error(const Error& error) noexcept;

 private:
  [[nodiscard]] VoidResult call_once(
      JSValueConst function,
      JSValueConst argument) noexcept;

  JSContext* ctx_{nullptr};
  vacps::qjs::OwnedValue resolve_;
  vacps::qjs::OwnedValue reject_;
  bool settled_{false};
};

}  // namespace vacps::runtime
