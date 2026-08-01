#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "app/platform.hpp"
#include "app/version.hpp"
#include "quickjs/module_bindings.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <quickjs.h>

namespace vacps::js {
namespace {

// vacps:host — thin process info only (not HTTP/SQL/process/fs).
JSValue js_host_version(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return converter<std::string>::to_js(ctx, std::string{vacps::version()}).release();
}

JSValue js_host_data_dir(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "host: not initialized");
  return converter<std::string>::to_js(ctx, host->services().data_dir).release();
}

JSValue js_host_now_ms(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  using clock = std::chrono::system_clock;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock::now().time_since_epoch())
                      .count();
  return JS_NewInt64(ctx, static_cast<std::int64_t>(ms));
}

JSValue js_host_platform(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return converter<std::string>::to_js(ctx, std::string{vacps::platform_string()}).release();
}

/**
 * host.getenv(name) → string | undefined
 * Reads ScriptServices.environment only (bootstrap snapshot).
 * Set empty → ""; unset → undefined (n1.md §十).
 */
JSValue js_host_getenv(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "host.getenv(name)");
  auto name = converter<std::string>::from_js(ctx, argv[0]);
  if (!name) return throw_error(ctx, name.error());
  if (name->empty() || name->find('\0') != std::string::npos) {
    return JS_ThrowTypeError(ctx, "host.getenv: invalid name");
  }
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "host: not initialized");
  auto v = host->services().environment.get(*name);
  if (!v) {
    return JS_UNDEFINED;
  }
  return converter<std::string>::to_js(ctx, std::move(*v)).release();
}

const JSCFunctionListEntry k_host_exports[] = {
    JS_CFUNC_DEF("version", 0, js_host_version),
    JS_CFUNC_DEF("dataDir", 0, js_host_data_dir),
    JS_CFUNC_DEF("nowMs", 0, js_host_now_ms),
    JS_CFUNC_DEF("platform", 0, js_host_platform),
    JS_CFUNC_DEF("getenv", 1, js_host_getenv),
};

int js_host_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_host_exports, VACPS_COUNTOF(k_host_exports));
}


}  // namespace

JSModuleDef* init_module_host(JSContext* ctx, const char* name, void* binding) {
  // binding: HostBindingContext* (data_dir / environment snapshot).
  [[maybe_unused]] auto* host_ctx = static_cast<HostBindingContext*>(binding);
  JSModuleDef* m = JS_NewCModule(ctx, name, js_host_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_host_exports, VACPS_COUNTOF(k_host_exports));
  return m;
}


}  // namespace vacps::js
