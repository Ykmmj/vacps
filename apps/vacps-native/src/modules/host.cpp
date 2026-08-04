#include "modules/bindings.hpp"

#include "app/platform.hpp"
#include "app/version.hpp"
#include "binding/env.hpp"
#include "binding/function.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

/** Named ES exports — declare at module create, set during init. */
constexpr const char* k_host_exports[] = {
    "version",
    "platform",
    "dataDir",
    "nowMs",
    "getenv",
};

/**
 * Module init (phase 2): create function values and JS_SetModuleExport each name.
 * No C++ exception may escape this C callback.
 *
 * data_dir is recovered from the mandatory JSRuntime composition opaque.
 */
int initialize_host(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    // Pure synchronous module: Env from live JSContext only.
    binding::Env env{ctx};
    binding::ModuleBuilder mod{env};

    // Snapshot host-wired data_dir at init (composition always installed).
    std::string data_dir = composition_from_context(ctx).data_dir;

    auto export_fn = [&](const char* name, auto fn, int length) -> bool {
      qjs::OwnedValue func =
          binding::create_function(env, name, std::move(fn), length);
      return mod.set_export(m, name, std::move(func)) == 0;
    };

    if (!export_fn(
            "version",
            []() -> std::string { return vacps::version(); },
            0)) {
      return -1;
    }
    if (!export_fn(
            "platform",
            []() -> std::string { return vacps::platform_string(); },
            0)) {
      return -1;
    }
    if (!export_fn(
            "dataDir",
            [data_dir]() -> std::string { return data_dir; },
            0)) {
      return -1;
    }
    if (!export_fn(
            "nowMs",
            []() -> std::int64_t {
              using clock = std::chrono::system_clock;
              return std::chrono::duration_cast<std::chrono::milliseconds>(
                         clock::now().time_since_epoch())
                  .count();
            },
            0)) {
      return -1;
    }
    // Unset → undefined (product types); set empty → "".
    // Env is injected by the binding DSL; name is decoded as a strict string.
    if (!export_fn(
            "getenv",
            [](binding::Env env,
               std::string name) -> binding::Result<qjs::OwnedValue> {
              // Reject empty / embedded-NUL names (not valid process env keys).
              if (name.empty() || name.find('\0') != std::string::npos) {
                return std::unexpected(
                    binding::Error::type("getenv: invalid name"));
              }
              const char* raw = std::getenv(name.c_str());
              if (raw == nullptr) {
                return env.undefined();
              }
              return env.string(raw);
            },
            1)) {
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
      (void)binding::throw_internal(ctx, "host module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_host(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_host);
    if (m == nullptr) {
      return nullptr;
    }

    for (const char* export_name : k_host_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "host module: declare_export failed");
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
      (void)binding::throw_internal(ctx, "host module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
