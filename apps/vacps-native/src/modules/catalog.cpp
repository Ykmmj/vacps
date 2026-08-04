#include "modules/catalog.hpp"

#include "modules/bindings.hpp"

#include <cstring>
#include <utility>

namespace vacps::js {

namespace {

[[nodiscard]] bool starts_with_vacps(const char* name) noexcept {
  static constexpr char kPrefix[] = "vacps:";
  return std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) == 0;
}

}  // namespace

ModuleCatalog::ModuleCatalog(
    Runtime::Async& async_runtime,
    Runtime::Callbacks& callbacks_runtime,
    process::ProcessRuntime& process_runtime,
    std::string data_dir,
    std::string ca_bundle)
    : composition_(
          async_runtime,
          callbacks_runtime,
          process_runtime,
          std::move(data_dir),
          std::move(ca_bundle)),
      modules_({
          {"vacps:crypto", init_module_crypto},
          {"vacps:host", init_module_host},
          {"vacps:log", init_module_log},
          {"vacps:store", init_module_store},
          {"vacps:fs", init_module_fs},
          {"vacps:http", init_module_http},
          {"vacps:process", init_module_process},
      }) {}

const ModuleDescriptor* ModuleCatalog::find(
    std::string_view specifier) const noexcept {
  for (const ModuleDescriptor& d : modules_) {
    if (d.specifier == specifier) {
      return &d;
    }
  }
  return nullptr;
}

JSModuleDef* ModuleCatalog::load_module(
    JSContext* ctx,
    const char* module_name,
    void* opaque) {
  auto* catalog = static_cast<const ModuleCatalog*>(opaque);

  const ModuleDescriptor* desc = catalog->find(module_name);
  if (desc == nullptr) {
    // Wide loader outcome: unknown specifier is a real JS ReferenceError.
    if (starts_with_vacps(module_name)) {
      JS_ThrowReferenceError(
          ctx,
          "unknown vacps module: %s (not registered in ModuleCatalog)",
          module_name);
    } else {
      JS_ThrowReferenceError(ctx, "module not found: %s", module_name);
    }
    return nullptr;
  }

  return desc->load(ctx, module_name);
}

void ModuleCatalog::install_loader(JSRuntime* rt) {
  // JSRuntime opaque carries runtime-scoped module composition
  // (Runtime::Async / Runtime::Callbacks / ProcessRuntime / data_dir /
  // ca_bundle).
  // C-module init callbacks only get (JSContext*, JSModuleDef*); they recover
  // composition via composition_from_context / async_runtime_from_context /
  // callbacks_runtime_from_context / process_runtime_from_context.
  // Do not use process globals, static maps, or thread_local. Do not overwrite
  // JS_SetContextOpaque (vacps::Runtime* owns that slot).
  JS_SetRuntimeOpaque(rt, &composition_);
  JS_SetModuleLoaderFunc(
      rt,
      /*module_normalize=*/nullptr,
      &ModuleCatalog::load_module,
      this);
}

RuntimeModuleComposition& composition_from_context(JSContext* ctx) noexcept {
  JSRuntime* rt = JS_GetRuntime(ctx);
  // JSContext opaque remains vacps::Runtime*; composition lives on JSRuntime.
  return *static_cast<RuntimeModuleComposition*>(JS_GetRuntimeOpaque(rt));
}

Runtime::Async& async_runtime_from_context(JSContext* ctx) noexcept {
  return composition_from_context(ctx).async_runtime;
}

Runtime::Callbacks& callbacks_runtime_from_context(JSContext* ctx) noexcept {
  return composition_from_context(ctx).callbacks_runtime;
}

process::ProcessRuntime& process_runtime_from_context(
    JSContext* ctx) noexcept {
  return composition_from_context(ctx).process_runtime;
}

}  // namespace vacps::js
