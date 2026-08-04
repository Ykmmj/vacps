#pragma once

/**
 * Shared JS encode concept for Runtime::Async (and any owner-thread encode path).
 *
 * Encode callables run on the JS owner thread and produce a
 * Result<vacps::qjs::OwnedValue>. Failures are C++ Errors (engine left clean);
 * they must not leave a pending QuickJS exception for the caller to settle.
 */

#include "qjs/owned_value.hpp"
#include "runtime/error.hpp"

#include <quickjs.h>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace vacps::runtime {

/** Encode T into a JS Value Result on the main / owner thread. */
template <class Encode, class T>
concept JsEncode = requires(
    std::decay_t<Encode>& encode,
    JSContext* ctx,
    T&& value) {
  {
    std::invoke(encode, ctx, std::forward<T>(value))
  } -> std::same_as<Result<vacps::qjs::OwnedValue>>;
};

}  // namespace vacps::runtime
