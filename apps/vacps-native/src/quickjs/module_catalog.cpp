#include "quickjs/module_catalog.hpp"

#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/globals/install.hpp"

#include <cstring>
#include <format>
#include <string>

namespace vacps::js {

namespace {

[[nodiscard]] bool starts_with_vacps(const char* name) noexcept {
  static constexpr char kPrefix[] = "vacps:";
  return name != nullptr && std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) == 0;
}

/** Process-static catalog; descriptors are function pointers only (no ScriptRuntime state). */
ModuleCatalog& default_module_catalog() {
  static ModuleCatalog catalog = [] {
    ModuleCatalog c;
    // Explicit composition list — add new vacps:* modules here only.
    auto reg = c.register_modules({
        {"vacps:log", init_module_log, nullptr},
        {"vacps:store", init_module_store, nullptr},
        {"vacps:host", init_module_host, nullptr},
        {"vacps:http", init_module_http, nullptr},
        {"vacps:fs", init_module_fs, nullptr},
        {"vacps:process", init_module_process, nullptr},
        {"vacps:crypto", init_module_crypto, nullptr},
    });
    if (!reg) {
      // Static init cannot return expected; leave empty — install will fail closed.
      return ModuleCatalog{};
    }
    return c;
  }();
  return catalog;
}

}  // namespace

VoidResult ModuleCatalog::register_module(ModuleDescriptor desc) {
  if (desc.specifier.empty()) {
    return std::unexpected(Error{"ModuleCatalog: empty specifier"});
  }
  if (desc.load == nullptr) {
    return std::unexpected(Error{
        std::format("ModuleCatalog: null load for '{}'", desc.specifier)});
  }
  if (find(desc.specifier) != nullptr) {
    return std::unexpected(Error{
        std::format("ModuleCatalog: duplicate specifier '{}'", desc.specifier)});
  }
  modules_.push_back(desc);
  return {};
}

VoidResult ModuleCatalog::register_modules(
    std::initializer_list<ModuleDescriptor> descs) {
  for (const ModuleDescriptor& d : descs) {
    if (auto r = register_module(d); !r) {
      return r;
    }
  }
  return {};
}

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
  if (ctx == nullptr || module_name == nullptr) {
    return nullptr;
  }
  auto* catalog = static_cast<const ModuleCatalog*>(opaque);
  if (catalog == nullptr) {
    JS_ThrowReferenceError(ctx, "module loader: catalog not installed");
    return nullptr;
  }

  const ModuleDescriptor* desc = catalog->find(module_name);
  if (desc == nullptr) {
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

  return desc->load(ctx, module_name, desc->binding);
}

void ModuleCatalog::install_loader(JSRuntime* rt) const {
  install_module_loader(rt, this);
}

void install_module_loader(JSRuntime* rt, const ModuleCatalog* catalog) {
  if (rt == nullptr) {
    return;
  }
  JS_SetModuleLoaderFunc(
      rt,
      /*module_normalize=*/nullptr,
      &ModuleCatalog::load_module,
      // QuickJS stores opaque as void*; catalog must outlive the runtime.
      const_cast<ModuleCatalog*>(catalog));
}

VoidResult install_modules(JSRuntime* rt, JSContext* ctx) {
  if (rt == nullptr || ctx == nullptr) {
    return std::unexpected(Error{"install_modules: null runtime/context"});
  }
  ModuleCatalog& catalog = default_module_catalog();
  if (catalog.size() == 0) {
    return std::unexpected(Error{"install_modules: module catalog empty"});
  }
  // Catalog is process-static and outlives every JSRuntime.
  install_module_loader(rt, &catalog);
  if (auto globals = install_global_apis(ctx); !globals) {
    return globals;
  }
  return {};
}

}  // namespace vacps::js
