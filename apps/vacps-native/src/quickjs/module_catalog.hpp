#pragma once

#include "app/error.hpp"

#include <quickjs.h>

#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace vacps::js {

/**
 * Describes one ES module (specifier → C loader).
 * `binding` is optional opaque for future Binding instances; may be nullptr.
 */
struct ModuleDescriptor {
  std::string_view specifier;
  JSModuleDef* (*load)(JSContext* ctx, const char* name, void* binding);
  void* binding{nullptr};
};

/**
 * Explicit registry of vacps:* (and other) modules.
 * Lookup by specifier; used as JS_SetModuleLoaderFunc opaque.
 * Does not create business objects.
 */
class ModuleCatalog {
 public:
  ModuleCatalog() = default;

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
   * Catalog must outlive the runtime (e.g. static or ScriptRuntime-owned).
   */
  void install_loader(JSRuntime* rt) const;

 private:
  std::vector<ModuleDescriptor> modules_;
};

/**
 * Install @p catalog as the JS module loader for @p rt.
 * Catalog address is stored as loader opaque — must outlive the runtime.
 */
void install_module_loader(JSRuntime* rt, const ModuleCatalog* catalog);

/**
 * Register vacps:* module loader and install global APIs.
 * ScriptRuntime is JS_GetContextOpaque. Used by ScriptRuntime::create.
 */
[[nodiscard]] VoidResult install_modules(JSRuntime* rt, JSContext* ctx);

}  // namespace vacps::js
