#pragma once

#include "app/config.hpp"
#include "app/error.hpp"
#include "process/registry.hpp"
#include "quickjs/context.hpp"
#include "quickjs/convert.hpp"
#include "quickjs/runtime.hpp"
#include "quickjs/value.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstdint>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <string_view>

namespace vacps::js {

namespace asio = boost::asio;

struct HostOptions {
  std::size_t heap_limit_bytes{kDefaultHeapLimitBytes};
  std::size_t stack_limit_bytes{kDefaultStackLimitBytes};
};

/**
 * QuickJS + Asio host infrastructure only:
 * - Runtime / Context ownership
 * - Load business ESM + invoke_export + Promise await (await_settled)
 * - notify_progress for Asio→JS resume
 *
 * Product capabilities live in vacps:* modules (http.Server, store, process, fs, …).
 * Process config is Host::config(); not a separate HostState bag.
 */
class Host : public std::enable_shared_from_this<Host> {
 public:
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  ~Host();

  [[nodiscard]] static Result<std::shared_ptr<Host>> create(
      const Config& cfg,
      asio::io_context& ioc,
      HostOptions opts = {});

  [[nodiscard]] bool ok() const noexcept { return runtime_.ok() && context_.ok(); }
  [[nodiscard]] bool script_ready() const noexcept { return script_initialized_; }
  [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
  [[nodiscard]] Context& context() noexcept { return context_; }
  [[nodiscard]] asio::io_context& ioc() noexcept { return *ioc_; }
  /** Blocking FS/metadata offload; content fallback when io_uring unusable. */
  [[nodiscard]] asio::thread_pool& pool() noexcept { return *pool_; }
  /**
   * True only if probe_io_uring() passed (setup+NOP+wait). When false, vacps:fs
   * content I/O uses thread_pool (Docker default seccomp, kernel disable, etc.).
   */
  [[nodiscard]] bool use_stream_file() const noexcept { return use_stream_file_; }
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }

  /** Long-lived subprocess registry (vacps:process start/read/write/terminate). */
  [[nodiscard]] vacps::process::Registry& processes() noexcept { return *processes_; }

  [[nodiscard]] Result<Value> eval(
      std::string_view source,
      std::string_view filename = "<eval>",
      int flags = JS_EVAL_TYPE_GLOBAL);

  [[nodiscard]] Result<Value> eval_module(
      std::string_view source,
      std::string_view filename = "<module>");

  [[nodiscard]] VoidResult drain_jobs() { return runtime_.drain_jobs(); }

  /**
   * Wake await_settled after Asio-side work resolves a JS Promise.
   * Bumps progress_generation_ then cancels progress waiters (event version).
   */
  void notify_progress();

  [[nodiscard]] std::uint64_t progress_generation() const noexcept {
    return progress_generation_;
  }

  void cancel_host_async() noexcept;

  [[nodiscard]] asio::awaitable<VoidResult> load_and_initialize(std::string_view script_path);
  [[nodiscard]] asio::awaitable<VoidResult> shutdown_script();

  /** Call a business-module export; awaits Promise results. */
  [[nodiscard]] asio::awaitable<Result<Value>> invoke_export(
      const char* name,
      int argc,
      JSValueConst* argv);

  /** Await a Promise (or pass through non-promise values). */
  [[nodiscard]] asio::awaitable<Result<Value>> await_value(Value value);

 private:
  Host(Runtime runtime, Context context, asio::io_context& ioc, Config cfg);

  [[nodiscard]] asio::awaitable<void> wait_progress();
  [[nodiscard]] asio::awaitable<VoidResult> await_settled(Value& value);

  Runtime runtime_;
  Context context_;
  asio::io_context* ioc_{nullptr};
  std::unique_ptr<asio::thread_pool> pool_;
  std::unique_ptr<vacps::process::Registry> processes_;
  bool use_stream_file_{false};
  Config cfg_{};
  bool script_initialized_{false};
  bool shutting_down_{false};

  Value script_ns_;
  std::list<std::shared_ptr<asio::steady_timer>> progress_waiters_;
  /** Monotonic progress event version for wait_progress / multi-waiter safety. */
  std::uint64_t progress_generation_{0};
};

/** JSContext opaque → Host* (set in Host::create). */
[[nodiscard]] inline Host* host_from(JSContext* ctx) noexcept {
  return static_cast<Host*>(JS_GetContextOpaque(ctx));
}

}  // namespace vacps::js
