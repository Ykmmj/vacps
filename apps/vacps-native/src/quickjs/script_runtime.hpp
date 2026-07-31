#pragma once

#include "app/error.hpp"
#include "quickjs/context.hpp"
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
 * Owns: JSRuntime, JSContext, script namespace, job drain, progress waiters,
 *       async-op scope, interrupt budget.
 * Does not own: thread_pool, process::Registry, data_dir, ca_bundle —
 * those live on ScriptServices (ApplicationRuntime composition root).
 *
 * Context opaque is ScriptRuntime* only (bindings reach services via services()).
 */
class ScriptRuntime : public std::enable_shared_from_this<ScriptRuntime> {
 public:
  ScriptRuntime(const ScriptRuntime&) = delete;
  ScriptRuntime& operator=(const ScriptRuntime&) = delete;
  ~ScriptRuntime();

  /**
   * @param engine    QuickJS limits / interrupt budget
   * @param services  Process services from ApplicationRuntime (required)
   */
  [[nodiscard]] static Result<std::shared_ptr<ScriptRuntime>> create(
      asio::io_context& ioc,
      EngineOptions engine,
      std::shared_ptr<ScriptServices> services);

  [[nodiscard]] bool ok() const noexcept { return runtime_.ok() && context_.ok(); }
  [[nodiscard]] bool script_ready() const noexcept { return script_initialized_; }
  [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
  [[nodiscard]] Context& context() noexcept { return context_; }
  [[nodiscard]] asio::io_context& ioc() noexcept { return *ioc_; }

  /** Process services injected at create (pool, Registry, data_dir, …). */
  [[nodiscard]] ScriptServices& services() noexcept { return *services_; }
  [[nodiscard]] const ScriptServices& services() const noexcept { return *services_; }

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

  [[nodiscard]] VoidResult drain_jobs() { return runtime_.drain_jobs(); }

  void notify_progress();

  [[nodiscard]] std::uint64_t progress_generation() const noexcept {
    return progress_generation_;
  }

  [[nodiscard]] std::chrono::milliseconds js_time_budget() const noexcept {
    return js_time_budget_;
  }

  void cancel_host_async() noexcept;

  [[nodiscard]] asio::awaitable<VoidResult> load_and_initialize(std::string_view script_path);
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
  std::chrono::milliseconds js_time_budget_{kDefaultJsTimeBudget};
  bool script_initialized_{false};
  bool shutting_down_{false};

  Value script_ns_;
  std::list<std::shared_ptr<asio::steady_timer>> progress_waiters_;
  std::uint64_t progress_generation_{0};
  std::size_t outstanding_async_{0};
};

[[nodiscard]] inline ScriptRuntime* script_runtime_from(JSContext* ctx) noexcept {
  return static_cast<ScriptRuntime*>(JS_GetContextOpaque(ctx));
}

}  // namespace vacps::js
