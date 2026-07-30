#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/** Register vacps:* module loader. Host is JS_GetContextOpaque. */
[[nodiscard]] VoidResult install_native_modules(JSRuntime* rt, JSContext* ctx);

}  // namespace vacps::js
