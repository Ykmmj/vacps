#pragma once

#include "app/error.hpp"
#include "quickjs/module_bindings.hpp"

#include <quickjs.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>

namespace vacps::js {

/**
 * Describes one ES module (specifier → C loader).
 * `binding` points at a ModuleBindings context (FsBindingContext*, …)
 * or nullptr for pure modules (log, crypto) that need no composition state.
 */
struct ModuleDescriptor {
  std::string_view specifier;
  JSModuleDef* (*load)(JSContext* ctx, const char* name, void* binding);
  void* binding{nullptr};
};

/**
 * Explicit registry of vacps:* (and other) modules.
 * Lookup by specifier; used as JS_SetModuleLoaderFunc opaque.
 * Instance-owned (ScriptRuntime); not a process-static singleton.
 * Does not create business objects (File / Store / Server / Process).
 */
class ModuleCatalog {
 public:
  ModuleCatalog() = default;

  ModuleCatalog(const ModuleCatalog&) = delete;
  ModuleCatalog& operator=(const ModuleCatalog&) = delete;
  ModuleCatalog(ModuleCatalog&&) = default;
  ModuleCatalog& operator=(ModuleCatalog&&) = default;

  /** Append descriptors. Fails if a specifier is already registered. */
  [[nodiscard]] VoidResult register_module(ModuleDescriptor desc);
  [[nodiscard]] VoidResult register_modules(
      std::initializer_list<ModuleDescriptor> descs);

  [[nodiscard]] const ModuleDescriptor* find(std::string_view specifier) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

  /**
   * JSModuleLoaderFunc-compatible thunk.
   * Opaque must be ModuleCatalog*.
   * Unknown vacps:* → clear ReferenceError; other missing → module not found.
   */
  static JSModuleDef* load_module(
      JSContext* ctx,
      const char* module_name,
      void* opaque);

  /**
   * Install this catalog as the runtime module loader
   * (JS_SetModuleLoaderFunc with load_module thunk).
   * Catalog must outlive the JSRuntime (ScriptRuntime-owned).
   */
  void install_loader(JSRuntime* rt) const;

 private:
  std::vector<ModuleDescriptor> modules_;
};

/**
 * Build the default vacps:* catalog with real binding pointers from @p bindings.
 * Explicit composition list — add new modules here only.
 */
[[nodiscard]] Result<std::unique_ptr<ModuleCatalog>> make_default_module_catalog(
    ModuleBindings& bindings);

/**
 * Install @p catalog as the JS module loader for @p rt.
 * Catalog address is stored as loader opaque — must outlive the runtime.
 */
void install_module_loader(JSRuntime* rt, const ModuleCatalog* catalog);

/**
 * Install module loader from @p catalog and global APIs on @p ctx.
 * Catalog must be non-null, non-empty, and outlive @p rt.
 * Prefer install_default_modules(ScriptRuntime&) from the composition root.
 */
[[nodiscard]] VoidResult install_modules(
    JSRuntime* rt,
    JSContext* ctx,
    ModuleCatalog* catalog);

}  // namespace vacps::js
