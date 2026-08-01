#include "quickjs/module_catalog.hpp"

#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/globals/install.hpp"
#include "quickjs/module_bindings.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/script_services.hpp"

#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace vacps::js {

void ModuleBindings::wire_from(ScriptServices& services) noexcept {
  fs.data_dir = &services.data_dir;
  fs.pool = &services.fs_pool;
  fs.use_asio_file = &services.use_asio_file;

  host.data_dir = &services.data_dir;
  host.environment = &services.environment;

  store.pool = &services.db_pool;

  http.ca_bundle = &services.ca_bundle;

  process.processes = &services.processes;
}

namespace {

[[nodiscard]] bool starts_with_vacps(const char* name) noexcept {
  static constexpr char kPrefix[] = "vacps:";
  return name != nullptr && std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) == 0;
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

Result<std::unique_ptr<ModuleCatalog>> make_default_module_catalog(
    ModuleBindings& bindings) {
  auto catalog = std::make_unique<ModuleCatalog>();
  // Explicit composition list — add new vacps:* modules here only.
  // Pure modules (log, crypto): binding = nullptr (no ScriptServices config).
  // Stateful modules: binding → ModuleBindings context (non-owning into ScriptServices).
  auto reg = catalog->register_modules({
      {"vacps:log", init_module_log, nullptr},
      {"vacps:store", init_module_store, &bindings.store},
      {"vacps:host", init_module_host, &bindings.host},
      {"vacps:http", init_module_http, &bindings.http},
      {"vacps:fs", init_module_fs, &bindings.fs},
      {"vacps:process", init_module_process, &bindings.process},
      {"vacps:crypto", init_module_crypto, nullptr},
  });
  if (!reg) {
    return std::unexpected(std::move(reg.error()));
  }
  return catalog;
}

VoidResult install_modules(
    JSRuntime* rt,
    JSContext* ctx,
    ModuleCatalog* catalog) {
  if (rt == nullptr || ctx == nullptr) {
    return std::unexpected(Error{"install_modules: null runtime/context"});
  }
  if (catalog == nullptr) {
    return std::unexpected(Error{"install_modules: null catalog"});
  }
  if (catalog->size() == 0) {
    return std::unexpected(Error{"install_modules: module catalog empty"});
  }
  // Catalog is owned by ScriptRuntime and outlives this JSRuntime.
  install_module_loader(rt, catalog);
  if (auto globals = install_global_apis(ctx); !globals) {
    return globals;
  }
  return {};
}

VoidResult install_default_modules(ScriptRuntime& rt) {
  if (rt.closed() || !rt.ok()) {
    return std::unexpected(Error{"install_default_modules: runtime not open"});
  }
  // Wire non-owning binding contexts into ScriptServices (composition).
  rt.bindings_.wire_from(*rt.services_);
  auto catalog = make_default_module_catalog(rt.bindings_);
  if (!catalog) {
    return std::unexpected(std::move(catalog.error()));
  }
  rt.catalog_ = std::move(*catalog);
  return install_modules(rt.runtime().get(), rt.context().get(), rt.catalog_.get());
}

}  // namespace vacps::js
