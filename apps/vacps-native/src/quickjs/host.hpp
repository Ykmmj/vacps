#pragma once

#include "app/error.hpp"
#include "quickjs/context.hpp"
#include "quickjs/convert.hpp"
#include "quickjs/module_env.hpp"
#include "quickjs/runtime.hpp"
#include "quickjs/value.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

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
 * QuickJS + Asio glue only:
 * - runtime/context, eval, invoke_export, await_settled
 * - one blocking thread_pool for offload
 * - async scope / progress waiters
 *
 * Capability backends live in ModuleEnv (owned for process lifetime,
 * accessed via env_from(ctx) / host.env()).
 */
class Host : public std::enable_shared_from_this<Host> {
 public:
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  ~Host();

  /**
   * @param engine  QuickJS limits / interrupt budget
   * @param modules capability backends (data_dir, sandbox roots, CA, …)
   */
  [[nodiscard]] static Result<std::shared_ptr<Host>> create(
      asio::io_context& ioc,
      EngineOptions engine = {},
      ModuleEnvOptions modules = {});

  [[nodiscard]] bool ok() const noexcept { return runtime_.ok() && context_.ok(); }
  [[nodiscard]] bool script_ready() const noexcept { return script_initialized_; }
  [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
  [[nodiscard]] Context& context() noexcept { return context_; }
  [[nodiscard]] asio::io_context& ioc() noexcept { return *ioc_; }
  /** Single process-wide blocking offload pool (not product-specific). */
  [[nodiscard]] asio::thread_pool& pool() noexcept { return *pool_; }

  /** Capability backends for vacps:* modules. */
  [[nodiscard]] ModuleEnv& env() noexcept { return *env_; }
  [[nodiscard]] const ModuleEnv& env() const noexcept { return *env_; }

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
  Host(
      Runtime runtime,
      Context context,
      asio::io_context& ioc,
      EngineOptions engine,
      ModuleEnvOptions modules);

  [[nodiscard]] asio::awaitable<void> wait_progress();
  [[nodiscard]] asio::awaitable<bool> wait_progress_or_deadline();
  [[nodiscard]] asio::awaitable<VoidResult> await_settled(Value& value);

  Runtime runtime_;
  Context context_;
  asio::io_context* ioc_{nullptr};
  std::unique_ptr<asio::thread_pool> pool_;
  std::unique_ptr<ModuleEnv> env_;
  std::chrono::milliseconds js_time_budget_{kDefaultJsTimeBudget};
  bool script_initialized_{false};
  bool shutting_down_{false};

  Value script_ns_;
  std::list<std::shared_ptr<asio::steady_timer>> progress_waiters_;
  std::uint64_t progress_generation_{0};
  std::size_t outstanding_async_{0};
};

[[nodiscard]] inline Host* host_from(JSContext* ctx) noexcept {
  return static_cast<Host*>(JS_GetContextOpaque(ctx));
}

/** Module backends — never use Host as a grab-bag of product services. */
[[nodiscard]] inline ModuleEnv* env_from(JSContext* ctx) noexcept {
  auto* h = host_from(ctx);
  return h != nullptr ? &h->env() : nullptr;
}

}  // namespace vacps::js
