#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "app/log.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"

#include <quickjs.h>

#include <string>

namespace vacps::js {
namespace {

/** Coerce any JS value to a log string (message: unknown per n1.md §十). */
std::string coerce_log_message(JSContext* ctx, JSValueConst v) {
  auto cs = CString::from_value(ctx, v);
  if (cs.empty()) {
    // JS_ToCString failed (exception pending). Clear so logging does not poison the engine.
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
    return "[unprintable]";
  }
  return cs.str();
}

JSValue js_log_write(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv,
    int magic) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "log expects a message");
  }
  // Accept any type: JS_ToCString coerces numbers/objects/null/etc.
  const std::string msg = coerce_log_message(ctx, argv[0]);
  switch (magic) {
    case 0: vacps::log::trace("{}", msg); break;
    case 1: vacps::log::debug("{}", msg); break;
    case 2: vacps::log::info("{}", msg); break;
    case 3: vacps::log::warn("{}", msg); break;
    default: vacps::log::error("{}", msg); break;
  }
  return JS_UNDEFINED;
}

/** flush(): Promise<void> — resolve after vacps::log::flush() (n1.md §十). */
JSValue js_log_flush(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "log.flush: runtime not wired");
  }
  return spawn_js_promise(
      ctx,
      host,
      [](JSContext* /*c*/, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        vacps::log::flush();
        bridge.resolve_undefined();
        co_return;
      });
}

const JSCFunctionListEntry k_log_exports[] = {
    JS_CFUNC_MAGIC_DEF("trace", 1, js_log_write, 0),
    JS_CFUNC_MAGIC_DEF("debug", 1, js_log_write, 1),
    JS_CFUNC_MAGIC_DEF("info", 1, js_log_write, 2),
    JS_CFUNC_MAGIC_DEF("warn", 1, js_log_write, 3),
    JS_CFUNC_MAGIC_DEF("error", 1, js_log_write, 4),
    JS_CFUNC_DEF("flush", 0, js_log_flush),
};

int js_log_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_log_exports, VACPS_COUNTOF(k_log_exports));
}


}  // namespace

JSModuleDef* init_module_log(JSContext* ctx, const char* name, void* binding) {
  // Pure module: binding is intentionally nullptr (no ScriptServices config).
  // flush() still uses script_runtime_from for Promise bridge only.
  (void)binding;
  JSModuleDef* m = JS_NewCModule(ctx, name, js_log_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_log_exports, VACPS_COUNTOF(k_log_exports));
  return m;
}


}  // namespace vacps::js
