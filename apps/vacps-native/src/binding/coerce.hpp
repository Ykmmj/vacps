#pragma once

/**
 * WebIDL-style ToString coercion for product bindings.
 *
 * Accepts arbitrary JS values via JS_ToCStringLen (unlike Converter<string>,
 * which remains strict JS_IsString). On failure clears the pending exception
 * and returns nullopt so Result paths stay engine-clean.
 *
 * Callers that need a different exception policy (e.g. log placeholders)
 * should use vacps::qjs::ScopedCString::from_value directly.
 */

#include "binding/error.hpp"
#include "qjs/scoped_cstring.hpp"

#include <optional>
#include <string>

namespace vacps::binding {

[[nodiscard]] inline std::optional<std::string> try_coerce_string(
    JSContext* ctx,
    JSValueConst v) {
  auto cs = vacps::qjs::ScopedCString::from_value(ctx, v);
  if (cs.empty()) {
    clear_exception(ctx);
    return std::nullopt;
  }
  return cs.str();
}

}  // namespace vacps::binding
