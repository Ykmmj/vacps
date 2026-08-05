#pragma once

#include "runtime/runtime_fwd.hpp"

#include <quickjs.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::process {
class ProcessRuntime;
}

namespace vacps::host {
class Application;
}

namespace vacps::js {

/**
 * Describes one ES module (specifier → C loader).
 *
 * Host capabilities live in the mandatory JSRuntime composition opaque; a
 * parallel per-module binding channel does not exist.
 */
struct ModuleDescriptor {
  std::string_view specifier;
  JSModuleDef* (*load)(JSContext* ctx, const char* name);
};

/**
 * Runtime-scoped module composition state owned by ModuleCatalog.
 *
 * Contract: Narrow (construction / opaque install)
 * Preconditions: Application supplies its own live lifecycle reference plus
 * live Runtime::Async, Runtime::Callbacks, and ProcessRuntime references that
 * outlive the catalog for the Runtime lifetime.
 *
 * Installed as JSRuntime opaque so C-module init callbacks recover host-wired
 * capabilities from a live JSContext alone. Does not store Async/Callbacks by
 * value, PromiseCapability, JSValue, or JSContext — Runtime::Impl owns the
 * stable Async/Callbacks/Script capabilities. Synchronous binding callbacks
 * run directly in the owner-thread QuickJS turn (no Runtime sync facade).
 *
 * Lifetime: catalog outlives the Runtime facade in Application member order.
 * References point into Impl-owned / host-owned objects and remain valid for
 * the Runtime lifetime. After Runtime teardown / FreeContext they must not be
 * used.
 */
struct RuntimeModuleComposition {
  host::Application& application;
  Runtime::Async& async_runtime;
  Runtime::Callbacks& callbacks_runtime;
  process::ProcessRuntime& process_runtime;
  /** Host data directory for vacps:host dataDir() (default "data"). */
  std::string data_dir;
  /**
   * Host CA bundle path for vacps:http outbound TLS (injected into
   * module-scoped HTTP Client; not exposed to JavaScript). Empty → platform
   * defaults.
   */
  std::string ca_bundle;

  RuntimeModuleComposition(
      host::Application& application_in,
      Runtime::Async& async_runtime_in,
      Runtime::Callbacks& callbacks_runtime_in,
      process::ProcessRuntime& process_runtime_in,
      std::string data_dir_in = "data",
      std::string ca_bundle_in = {})
      : application(application_in),
        async_runtime(async_runtime_in),
        callbacks_runtime(callbacks_runtime_in),
        process_runtime(process_runtime_in),
        data_dir(
            data_dir_in.empty() ? std::string{"data"}
                                : std::move(data_dir_in)),
        ca_bundle(std::move(ca_bundle_in)) {}
};

/**
 * Explicit registry of vacps:* modules.
 * Instance-owned by Application; not a process-static singleton.
 *
 * Owns RuntimeModuleComposition and installs it as JSRuntime opaque in
 * install_loader(). Module loader opaque is the catalog itself (`this`), so
 * ModuleCatalog is noncopyable and immovable — the address must remain stable
 * for the JSRuntime lifetime.
 * Convention: JSRuntime opaque = runtime module composition; JSContext opaque
 * remains vacps::Runtime* (never overwritten here).
 */
class ModuleCatalog {
 public:
  /**
   * Contract: Narrow
   * Preconditions: application/async/callbacks/process outlive this catalog
   * for the Runtime lifetime; data_dir empty becomes "data".
   */
  explicit ModuleCatalog(
      host::Application& application,
      Runtime::Async& async_runtime,
      Runtime::Callbacks& callbacks_runtime,
      process::ProcessRuntime& process_runtime,
      std::string data_dir = "data",
      std::string ca_bundle = {});

  ModuleCatalog(const ModuleCatalog&) = delete;
  ModuleCatalog& operator=(const ModuleCatalog&) = delete;
  ModuleCatalog(ModuleCatalog&&) = delete;
  ModuleCatalog& operator=(ModuleCatalog&&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

  /**
   * QuickJS module loader entry.
   * Contract: Narrow for ctx/module_name/opaque (live loader install).
   * Unknown JavaScript module specifiers remain a Wide loader outcome and
   * throw ReferenceError.
   */
  static JSModuleDef* load_module(
      JSContext* ctx,
      const char* module_name,
      void* opaque);

  /**
   * Install this catalog as JS_SetModuleLoaderFunc opaque (address is stored;
   * catalog is immovable and must outlive JSRuntime) and install composition()
   * as JS_SetRuntimeOpaque for C-module init recovery.
   * Contract: Narrow — rt is the live product JSRuntime.
   * Does not call JS_SetContextOpaque.
   */
  void install_loader(JSRuntime* rt);

 private:
  [[nodiscard]] const ModuleDescriptor* find(
      std::string_view specifier) const noexcept;

  RuntimeModuleComposition composition_;
  std::vector<ModuleDescriptor> modules_;
};

/**
 * Recover RuntimeModuleComposition& from JSContext via JSRuntime opaque.
 * Contract: Narrow
 * Preconditions: ctx is the live runtime context; composition was installed
 * as JSRuntime opaque by ModuleCatalog::install_loader.
 * Does not read or write JSContext opaque.
 */
[[nodiscard]] RuntimeModuleComposition& composition_from_context(
    JSContext* ctx) noexcept;

/**
 * Recover Runtime::Async& from JSContext via installed composition.
 * Contract: Narrow (same as composition_from_context).
 */
[[nodiscard]] Runtime::Async& async_runtime_from_context(
    JSContext* ctx) noexcept;

/**
 * Recover Runtime::Callbacks& from JSContext via installed composition.
 * Contract: Narrow (same as composition_from_context).
 */
[[nodiscard]] Runtime::Callbacks& callbacks_runtime_from_context(
    JSContext* ctx) noexcept;

/**
 * Recover ProcessRuntime& from JSContext via installed composition.
 * Contract: Narrow (same as composition_from_context).
 */
[[nodiscard]] process::ProcessRuntime& process_runtime_from_context(
    JSContext* ctx) noexcept;

}  // namespace vacps::js
