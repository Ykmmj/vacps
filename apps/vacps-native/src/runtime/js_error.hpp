#pragma once

/**
 * Map runtime::Error → JavaScript Error object / throw.
 * Shared by owner-thread throw paths and PromiseCapability::reject_error.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

namespace vacps::runtime {

/** Stable string for Error.code on JS Error objects (e.g. ERR_NATIVE_FAILURE). */
[[nodiscard]] const char* error_code_name(Errc code) noexcept;

/**
 * Build a JS Error with message + code. Does not throw.
 * On failure returns unexpected (caller may fall back to a string reject).
 */
[[nodiscard]] Result<vacps::qjs::OwnedValue> make_js_error_object(
    JSContext* ctx,
    const Error& error) noexcept;

/**
 * Throw a JS exception for Error. Always returns JS_EXCEPTION.
 * Ownership of the error object is transferred to the exception state.
 */
[[nodiscard]] JSValue throw_js_error(
    JSContext* ctx,
    const Error& error) noexcept;

}  // namespace vacps::runtime
