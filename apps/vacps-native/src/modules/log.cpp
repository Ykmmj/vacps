#include "modules/bindings.hpp"

#include "app/log.hpp"
#include "binding/async_function.hpp"
#include "binding/function.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "qjs/scoped_cstring.hpp"
#include "runtime/runtime_async.hpp"

#include <quickjs.h>

#include <exception>
#include <new>
#include <stop_token>
#include <string>
#include <utility>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

/** Named ES exports — declare at module create, set during init. */
constexpr const char* k_log_exports[] = {
    "trace",
    "debug",
    "info",
    "warn",
    "error",
    "flush",
};

/** Coerce any JS value to a log string. */
std::string coerce_log_message(JSContext* ctx, JSValueConst v) {
  auto cs = vacps::qjs::ScopedCString::from_value(ctx, v);
  if (cs.empty()) {
    // JS_ToCString failed (exception pending). Clear so logging does not
    // poison the engine; log a placeholder instead.
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
    return "[unprintable]";
  }
  return cs.str();
}

void write_log_level(int level, const std::string& msg) {
  switch (level) {
    case 0:
      vacps::log::trace("{}", msg);
      break;
    case 1:
      vacps::log::debug("{}", msg);
      break;
    case 2:
      vacps::log::info("{}", msg);
      break;
    case 3:
      vacps::log::warn("{}", msg);
      break;
    default:
      vacps::log::error("{}", msg);
      break;
  }
}

/**
 * Module init (phase 2): create function values and JS_SetModuleExport each name.
 * No C++ exception may escape this C callback.
 *
 * Runtime::Async is recovered from the mandatory JSRuntime composition
 * opaque. Synchronous methods use Env from ctx; flush needs Async.
 */
int initialize_log(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder mod{env};

    auto export_level = [&](const char* name, int level) -> bool {
      qjs::OwnedValue func = binding::create_function(
          env,
          name,
          [level](const binding::CallbackInfo& info) -> binding::VoidResult {
            if (info.length() < 1) {
              return std::unexpected(
                  binding::Error::type("log expects a message"));
            }
            // Accept any type: JS_ToCString coerces numbers/objects/null/etc.
            const std::string msg =
                coerce_log_message(info.context(), info[0].get());
            write_log_level(level, msg);
            return {};
          },
          1);
      return mod.set_export(m, name, std::move(func)) == 0;
    };

    if (!export_level("trace", 0)) {
      return -1;
    }
    if (!export_level("debug", 1)) {
      return -1;
    }
    if (!export_level("info", 2)) {
      return -1;
    }
    if (!export_level("warn", 3)) {
      return -1;
    }
    if (!export_level("error", 4)) {
      return -1;
    }

    // Capture non-owning Runtime::Async* (stable for Runtime lifetime).
    // Do not capture JS handles. Runtime::Async is the sole Promise owner.
    qjs::OwnedValue flush_fn = binding::create_async_function(
        env,
        "flush",
        [async](std::stop_token stop) mutable -> runtime::Task<void> {
          co_return co_await async->run_blocking(stop, [] {
            vacps::log::flush();
          });
        },
        0);  // explicit length 0 (no JS argv; stop_token is not counted)
    if (mod.set_export(m, "flush", std::move(flush_fn)) != 0) {
      return -1;
    }

    return 0;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return -1;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return -1;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "log module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_log(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_log);
    if (m == nullptr) {
      return nullptr;
    }

    // Phase 1: declare every named export (required before SetModuleExport).
    for (const char* export_name : k_log_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "log module: declare_export failed");
        }
        return nullptr;
      }
    }
    return m;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return nullptr;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return nullptr;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "log module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
