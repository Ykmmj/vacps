#pragma once

#include "app/error.hpp"
#include "quickjs/context.hpp"
#include "quickjs/module_bindings.hpp"
#include "quickjs/module_catalog.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/runtime.hpp"
#include "quickjs/raii/value.hpp"
#include "quickjs/script_services.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::js {

namespace asio = boost::asio;

/** Engine knobs only (heap / stack / interrupt). Not product config. */
struct EngineOptions {
  std::size_t heap_limit_bytes{kDefaultHeapLimitBytes};
  std::size_t stack_limit_bytes{kDefaultStackLimitBytes};
  std::chrono::milliseconds js_time_budget{kDefaultJsTimeBudget};
};

/**
 * QuickJS engine + job/promise/interrupt glue (temp/n1.md ScriptRuntime).
 *
 * Owns: JSRuntime, JSContext, ModuleCatalog + ModuleBindings (after install),
 *       script namespace, job drain, progress waiters, async-op scope,
 *       interrupt budget.
 * Does not own: thread_pool, process::Registry, data_dir, ca_bundle —
 * those live on ScriptServices (ApplicationRuntime composition root);
 * ModuleBindings holds non-owning pointers into ScriptServices.
 *
 * Context opaque is ScriptRuntime* only (Promise bridge / services()).
 * ModuleDescriptor.binding points at ModuleBindings contexts (or nullptr).
 *
 * Layering: create() is engine-only (vacps_quickjs_core). vacps:* modules and
 * globals are installed by install_default_modules() in bindings, called from
 * ApplicationRuntime (or tests) — keeps core free of bindings link edges.
 */
class ScriptRuntime : public std::enable_shared_from_this<ScriptRuntime> {
 public:
  ScriptRuntime(const ScriptRuntime&) = delete;
  ScriptRuntime& operator=(const ScriptRuntime&) = delete;
  ~ScriptRuntime();

  /**
   * Create engine + context only (no vacps:* modules / globals / catalog).
   *
   * Call install_default_modules(*rt) from vacps_quickjs_bindings after create
   * (ApplicationRuntime::start does this; tests that import vacps:* or use
   * URL/Text* globals must too).
   *
   * @param engine    QuickJS limits / interrupt budget
   * @param services  Process services from ApplicationRuntime (required)
   */
  [[nodiscard]] static Result<std::shared_ptr<ScriptRuntime>> create(
      asio::io_context& ioc,
      EngineOptions engine,
      std::shared_ptr<ScriptServices> services);

  /**
   * Wire bindings, build default vacps:* catalog, install loader + globals.
   * Defined in vacps_quickjs_bindings (module_catalog.cpp) — not core.
   */
  friend VoidResult install_default_modules(ScriptRuntime& rt);

  [[nodiscard]] bool ok() const noexcept { return runtime_.ok() && context_.ok(); }
  [[nodiscard]] bool script_ready() const noexcept { return script_initialized_; }
  /**
   * True after cancel_host_async / shutdown_script / close().
   * HTTP ScriptRequestHandler returns 503 while closing so teardown can
   * drop the runtime without new script work racing in.
   */
  [[nodiscard]] bool closing() const noexcept {
    return shutting_down_ || closed_;
  }
  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
  [[nodiscard]] Context& context() noexcept { return context_; }
  [[nodiscard]] asio::io_context& ioc() noexcept { return *ioc_; }

  /** Process services injected at create (pool, Registry, data_dir, …). */
  [[nodiscard]] ScriptServices& services() noexcept { return *services_; }
  [[nodiscard]] const ScriptServices& services() const noexcept { return *services_; }

  /** Instance module catalog (loader opaque); valid after install_default_modules(). */
  [[nodiscard]] ModuleCatalog* module_catalog() noexcept { return catalog_.get(); }
  [[nodiscard]] const ModuleCatalog* module_catalog() const noexcept {
    return catalog_.get();
  }

  /** Binding contexts pointed to by ModuleDescriptor.binding. */
  [[nodiscard]] ModuleBindings& module_bindings() noexcept { return bindings_; }
  [[nodiscard]] const ModuleBindings& module_bindings() const noexcept {
    return bindings_;
  }

  void async_op_begin() noexcept;
  void async_op_end() noexcept;
  [[nodiscard]] std::size_t outstanding_async_ops() const noexcept {
    return outstanding_async_;
  }
  [[nodiscard]] asio::awaitable<void> wait_async_idle(
      std::chrono::milliseconds timeout = std::chrono::seconds{5});

  [[nodiscard]] Result<Value> eval(
      std::string_view source,
      std::string_view filename = "<eval>",
      int flags = JS_EVAL_TYPE_GLOBAL);

  [[nodiscard]] Result<Value> eval_module(
      std::string_view source,
      std::string_view filename = "<module>");

  [[nodiscard]] VoidResult drain_jobs() {
    if (closed_ || !runtime_.ok()) {
      return success();
    }
    return runtime_.drain_jobs();
  }

  void notify_progress();

  [[nodiscard]] std::uint64_t progress_generation() const noexcept {
    return progress_generation_;
  }

  [[nodiscard]] std::chrono::milliseconds js_time_budget() const noexcept {
    return js_time_budget_;
  }

  void cancel_host_async() noexcept;

  /**
   * Soft mark for process teardown (closing() becomes true) without cancelling
   * progress waiters or freeing the engine. Safe before wait_async_idle / JS
   * shutdown so new HTTP dispatch returns 503 while native ops can still settle.
   */
  void mark_stopping() noexcept { shutting_down_ = true; }

  /**
   * Free long-lived JS values, clear context opaque, JS_FreeContext,
   * JS_FreeRuntime. Idempotent. Does not stop ScriptServices pool or ioc.
   * Call after JS shutdown() + job drain (ShutdownCoordinator step).
   */
  void close() noexcept;

  /**
   * Compile + eval module from in-memory source, then call export initialize().
   * Does not touch the filesystem — callers (ApplicationRuntime / tests) read
   * the script file and pass source + filename for diagnostics.
   */
  [[nodiscard]] asio::awaitable<VoidResult> initialize_from_source(
      std::string_view source,
      std::string_view filename);

  [[nodiscard]] asio::awaitable<VoidResult> shutdown_script();

  [[nodiscard]] asio::awaitable<Result<Value>> invoke_export(
      const char* name,
      int argc,
      JSValueConst* argv);

  [[nodiscard]] asio::awaitable<Result<Value>> await_value(Value value);

 private:
  ScriptRuntime(
      Runtime runtime,
      Context context,
      asio::io_context& ioc,
      EngineOptions engine,
      std::shared_ptr<ScriptServices> services);

  [[nodiscard]] asio::awaitable<void> wait_progress();
  [[nodiscard]] asio::awaitable<bool> wait_progress_or_deadline();
  [[nodiscard]] asio::awaitable<VoidResult> await_settled(Value& value);

  Runtime runtime_;
  Context context_;
  asio::io_context* ioc_{nullptr};
  std::shared_ptr<ScriptServices> services_;
  /**
   * Binding contexts + catalog (destroy catalog before bindings before services).
   * Declaration order: services_ → bindings_ → catalog_ so reverse destruction
   * drops loader opaque first, then contexts, then ScriptServices fields.
   */
  ModuleBindings bindings_;
  std::unique_ptr<ModuleCatalog> catalog_;
  std::chrono::milliseconds js_time_budget_{kDefaultJsTimeBudget};
  bool script_initialized_{false};
  bool shutting_down_{false};
  bool closed_{false};

  Value script_ns_;
  std::list<std::shared_ptr<asio::steady_timer>> progress_waiters_;
  std::uint64_t progress_generation_{0};
  std::size_t outstanding_async_{0};
};

[[nodiscard]] inline ScriptRuntime* script_runtime_from(JSContext* ctx) noexcept {
  return static_cast<ScriptRuntime*>(JS_GetContextOpaque(ctx));
}

/**
 * Wire ModuleBindings, build default vacps:* catalog, install loader + globals.
 * Call after ScriptRuntime::create from composition root / tests.
 * Implemented in vacps_quickjs_bindings (module_catalog.cpp).
 */
[[nodiscard]] VoidResult install_default_modules(ScriptRuntime& rt);

/** Read script path as binary text (filesystem I/O outside ScriptRuntime). */
[[nodiscard]] Result<std::string> read_script_file(std::string_view path);

/**
 * Convenience: read_script_file + initialize_from_source (tests / helpers).
 * Process entry uses ApplicationRuntime which reads the file itself.
 */
[[nodiscard]] asio::awaitable<VoidResult> load_and_initialize(
    ScriptRuntime& rt,
    std::string_view script_path);

}  // namespace vacps::js
