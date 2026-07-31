#pragma once

#include "app/error.hpp"
#include "fs/sandbox.hpp"
#include "process/registry.hpp"
#include "quickjs/context.hpp"
#include "quickjs/convert.hpp"
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

/**
 * Construction knobs for Host. Callers (main / tests) fill these in;
 * Host and modules do not invent process policy from a Config bag.
 */
struct HostOptions {
  /** Agent state directory (relative fs paths, host.dataDir()). */
  std::string data_dir{"data"};
  /**
   * Optional default CA file for outbound https.request when the script
   * omits ca_bundle. Empty → platform CA resolution in the client.
   */
  std::string ca_bundle;
  std::size_t heap_limit_bytes{kDefaultHeapLimitBytes};
  std::size_t stack_limit_bytes{kDefaultStackLimitBytes};
  std::chrono::milliseconds js_time_budget{kDefaultJsTimeBudget};
  /** Extra PathSandbox roots beyond data_dir + /tmp. */
  std::vector<std::string> fs_extra_roots;
};

/**
 * QuickJS + Asio host: runtime/context, script load, Promise await, sandbox.
 * Product policy (listen, auth, CP keys) is JavaScript.
 */
class Host : public std::enable_shared_from_this<Host> {
 public:
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  ~Host();

  [[nodiscard]] static Result<std::shared_ptr<Host>> create(
      asio::io_context& ioc,
      HostOptions opts = {});

  [[nodiscard]] bool ok() const noexcept { return runtime_.ok() && context_.ok(); }
  [[nodiscard]] bool script_ready() const noexcept { return script_initialized_; }
  [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
  [[nodiscard]] Context& context() noexcept { return context_; }
  [[nodiscard]] asio::io_context& ioc() noexcept { return *ioc_; }
  /** Shared pool for blocking fs / non-serial work (size 2). */
  [[nodiscard]] asio::thread_pool& pool() noexcept { return *pool_; }
  /**
   * Serial SQLite executor (size 1). All vacps:store DB work must run here so
   * connections stay single-threaded and the JS io_context is never blocked.
   */
  [[nodiscard]] asio::thread_pool& db_pool() noexcept { return *db_pool_; }
  [[nodiscard]] bool use_stream_file() const noexcept { return use_stream_file_; }
  [[nodiscard]] const std::string& data_dir() const noexcept { return data_dir_; }
  [[nodiscard]] const std::string& ca_bundle() const noexcept { return ca_bundle_; }

  [[nodiscard]] vacps::process::Registry& processes() noexcept { return *processes_; }
  [[nodiscard]] const vacps::fs::PathSandbox& path_sandbox() const noexcept {
    return path_sandbox_;
  }

  /**
   * Process-level async scope: every spawn_js_promise begins an op; settle/end
   * leaves it. Graceful shutdown waits for outstanding ops before stopping ioc.
   */
  void async_op_begin() noexcept;
  void async_op_end() noexcept;
  [[nodiscard]] std::size_t outstanding_async_ops() const noexcept {
    return outstanding_async_;
  }
  /** Wait until outstanding_async_ops()==0 or timeout. */
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
      HostOptions opts,
      vacps::fs::PathSandbox path_sandbox);

  [[nodiscard]] asio::awaitable<void> wait_progress();
  [[nodiscard]] asio::awaitable<bool> wait_progress_or_deadline();
  [[nodiscard]] asio::awaitable<VoidResult> await_settled(Value& value);

  Runtime runtime_;
  Context context_;
  asio::io_context* ioc_{nullptr};
  std::unique_ptr<asio::thread_pool> pool_;
  std::unique_ptr<asio::thread_pool> db_pool_;
  std::unique_ptr<vacps::process::Registry> processes_;
  bool use_stream_file_{false};
  std::string data_dir_;
  std::string ca_bundle_;
  vacps::fs::PathSandbox path_sandbox_{};
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

}  // namespace vacps::js
