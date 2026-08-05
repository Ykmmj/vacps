#pragma once

/**
 * Runtime::Impl — sole Runtime implementation object (internal).
 *
 * Not a product type. Owned uniquely by vacps::Runtime via unique_ptr.
 * Included only by Runtime implementation translation units. Never exposed
 * through a product core()/state() accessor.
 *
 * Owns: Options, main io_context (JS owner thread), worker pool (pure C++),
 * JsEngine, one runtime-wide std::stop_source, daemon work_guard, bounded job
 * pump, and stable capability objects (Async / Callbacks / Script).
 *
 * Normal shutdown is Asio natural drain:
 *   request_stop → begin_shutdown (phase stopping + stop_source +
 *   release daemon work_guard) → main_io_.run() continues until no real
 *   outstanding work remains → close QuickJS → join workers.
 * Does NOT call io_context::stop()/restart()/poll() on the normal path, and
 * does not keep AsyncScope / ReverseAwaitTracker / JS cleanup registries /
 * late-completion drains / shutdown deadlines.
 *
 * Does NOT use shared ownership of the whole Runtime/Impl. Promise completion
 * lives in its completion handler, and run_blocking transports typed outcomes
 * through Asio completion; only reverse-await state shared with JS callbacks
 * uses operation-local shared ownership. Capabilities hold non-owning Impl
 * references.
 *
 * JS_SetContextOpaque stores the stable public Runtime* facade
 * (Host / modules must not overwrite).
 *
 * Contract: Narrow (full Runtime lifetime)
 * Preconditions:
 * - Runtime outlives run(), every reference from async()/callbacks()/script(),
 *   and every handler/coroutine submitted through those capabilities.
 * - After initialize() establishes the owner thread, Runtime is destroyed on
 *   that owner thread.
 * - Concurrent request_stop()/post_to_owner() calls do not race Runtime destruction.
 * Errors:
 * - No recoverable error is promised for lifetime, thread, context, or
 *   ownership violations.
 * Threading:
 * - Only the owner thread drives main_io and touches QuickJS.
 * - request_stop/post_to_owner may be called from other threads while Runtime remains
 *   alive.
 * Lifetime / teardown:
 * - run() installs the daemon work_guard, runs startup, and blocks on a single
 *   main_io_.run(). Stopping releases only the daemon guard; run returns when
 *   Asio reports no outstanding work. Engine close and worker join happen only
 *   after that natural return. Unexpected exceptions from main_io_.run() are
 *   not translated into a healthy Runtime result.
 * - Member declaration order (below) is load-bearing: main_io_ is declared
 *   before engine/capabilities so it is destroyed *after* them.
 * - Destructor preconditions are caller obligations and are not dynamically
 *   rechecked. Do not execute queued handlers during destruction. Do not call
 *   main_io.stop()/poll/restart.
 * - Remaining main_io frames destroy with main_io_ after engine/capabilities.
 *   No whole-Runtime shared ownership.
 */

#include "runtime/error.hpp"
#include "runtime/js_engine.hpp"
#include "runtime/runtime_async.hpp"
#include "runtime/runtime_callbacks.hpp"
#include "runtime/runtime_fwd.hpp"
#include "runtime/runtime_script.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>

namespace vacps::runtime::detail {
struct JsPromiseAwaitAccess;
}  // namespace vacps::runtime::detail

namespace vacps {

struct Runtime::Impl {
  /**
   * @param facade Stable public Runtime address (context opaque + composition).
   * @param options Owned configuration (no duplicate option type).
   */
  explicit Impl(Runtime& facade, Options options);
  ~Impl() noexcept;

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] Runtime& facade() noexcept { return facade_; }

  [[nodiscard]] Runtime::Async& async() noexcept { return async_; }
  [[nodiscard]] const Runtime::Async& async() const noexcept { return async_; }
  [[nodiscard]] Runtime::Callbacks& callbacks() noexcept { return callbacks_; }
  [[nodiscard]] const Runtime::Callbacks& callbacks() const noexcept {
    return callbacks_;
  }
  [[nodiscard]] Runtime::Script& script() noexcept { return script_; }
  [[nodiscard]] const Runtime::Script& script() const noexcept {
    return script_;
  }

  /**
   * created → initialized.
   *
   * Contract: Narrow
   * Preconditions: phase == created; called once.
   * Errors: Result for expected engine/allocation failure only — not for
   * repeated initialize or wrong-phase programmer misuse.
   * Threading: calling thread becomes the sole JS owner thread.
   * Does not enter the Asio loop.
   */
  [[nodiscard]] runtime::VoidResult initialize();

  /**
   * initialized → running, install daemon work_guard, run startup
   * while phase==running so top-level JS → native is legal, then block on
   * main_io until natural drain after stop. Call from the same thread as
   * initialize.
   *
   * Contract: Narrow
   * Preconditions: initialized; same owner thread as initialize; called once;
   * startup contains a callable target.
   * @return 0 on success / normal request_stop; non-zero on the first fatal
   * operational startup, main-loop, or post-drain failure (still closes
   * cleanly when possible). Programmer misuse of preconditions is not modeled
   * as a healthy integer failure code.
   */
  int run(std::move_only_function<runtime::VoidResult() noexcept> startup);

  /**
   * Any thread, idempotent. Never touches QuickJS here.
   * Stop intent is retained even if post fails or run has not started.
   * A call racing Runtime destruction violates the lifetime precondition.
   * Repeated/late stop intent is an expected state transition while alive.
   * Does not call main_io.stop(); post allocation failure fails fast.
   */
  void request_stop() noexcept;

  [[nodiscard]] runtime::asio::any_io_executor main_executor() noexcept {
    return main_io_.get_executor();
  }

  [[nodiscard]] runtime::asio::any_io_executor worker_executor() noexcept {
    return worker_pool_.get_executor();
  }

  /**
   * Runtime-wide cooperative shutdown stop token. Captured by value into
   * managed coroutine / reverse-await frames. request_stop makes it sticky.
   */
  [[nodiscard]] std::stop_token shutdown_stop_token() const noexcept {
    return shutdown_stop_source_.get_token();
  }

  /**
   * Post work onto the JS / main_io thread.
   * Safe from any thread while Runtime is alive. Does not expose
   * io_context::stop.
   *
   * Phase contract: accepts only initialized / running. Stopping and closed
   * reject new host JS work (operational Result). Handlers capture non-owning
   * Impl* under the Runtime lifetime contract and re-check admission on
   * execution — a queued callable observing stopping is dropped without
   * touching JS.
   *
   * Posted callable: must not throw. The exact owner-thread handler is
   * noexcept; an unexpected throw may terminate. There is no catch-all fault
   * barrier mapping programmer exceptions to healthy runtime continuation.
   *
   * Destruction contract: on a successful post, the callable is destroyed on
   * the JS owner thread (handler run or owner-thread main_io natural drain).
   * If this call returns an Error without posting, the callable is destroyed
   * on the calling thread — do not capture JS-owning state when invoking
   * post_to_owner off-owner without a guaranteed accepting phase.
   *
   * Empty callable is a Narrow programmer misuse and is not checked here.
   */
  [[nodiscard]] runtime::VoidResult post_to_owner(
      std::move_only_function<void() noexcept> f);

  /**
   * Live engine JSContext for host use.
   *
   * Contract: Narrow
   * Preconditions: owner thread; engine open; phase is
   * initialized | running | stopping.
   * Violations are programmer errors and are not checked here. No recoverable
   * off-owner / post-close nullptr product path is advertised.
   */
  [[nodiscard]] JSContext* context() noexcept;

  [[nodiscard]] Runtime::Phase phase() const noexcept {
    return phase_.load(std::memory_order_acquire);
  }

  /**
   * Evaluate JS on the JS owner thread with interrupt budget; schedule job
   * pump after success when the engine may run microtasks.
   *
   * Contract: Narrow
   * Preconditions: owner thread; engine open; phase is
   * initialized | running | stopping.
   * Errors: Result reports JavaScript evaluation / job-scheduling failures,
   * not wrong-thread or wrong-phase misuse.
   */
  [[nodiscard]] runtime::Result<vacps::qjs::OwnedValue> evaluate(
      std::string_view source,
      std::string_view filename,
      int flags);

  /**
   * Post one non-recursive job-pump turn onto main_io when QuickJS has work.
   * No pending job or already-scheduled is a no-op. Narrow: engine stays live
   * until main_io naturally drains all posted turns — no phase/engine probes.
   */
  void schedule_job_pump() noexcept;
  void report_error(const runtime::Error& error) noexcept;

  /** Friend-only engine access for capabilities and reverse-await. */
  [[nodiscard]] runtime::JsEngine& engine() noexcept { return engine_; }
  [[nodiscard]] const runtime::JsEngine& engine() const noexcept {
    return engine_;
  }

  [[nodiscard]] JSContext* owner_context() noexcept {
    return engine_.context();
  }

  [[nodiscard]] const Options& options() const noexcept { return options_; }

 private:
  friend class Runtime::Async;
  friend class Runtime::Script;
  friend class Runtime::Callbacks;
  friend struct runtime::detail::JsPromiseAwaitAccess;

  [[nodiscard]] bool accepts_owner_post() const noexcept {
    const auto p = phase();
    return p == Runtime::Phase::initialized || p == Runtime::Phase::running;
  }

  void begin_shutdown() noexcept;
  [[nodiscard]] runtime::VoidResult run_job_turn();
  void close_engine() noexcept;
  void release_daemon_work() noexcept;
  void join_workers() noexcept;
  void note_fatal(const runtime::Error& error) noexcept;

  using WorkGuard =
      runtime::asio::executor_work_guard<runtime::asio::io_context::executor_type>;

  // ── Member order is load-bearing (see file header lifetime / teardown) ──
  // C++ destroys in reverse declaration order after the destructor body.
  //
  // main_io_ is declared *before* engine/capabilities so it is destroyed
  // *after* them.
  //
  // Destructor body releases the daemon guard and closes an open engine.
  // Owner-thread and quiescence requirements are caller preconditions.
  //   Do not stop/poll/restart main_io or execute queued handlers.
  // Residual frames destroy with main_io_ last among subsystems.

  Runtime& facade_;
  Options options_;
  runtime::asio::io_context main_io_{1};
  runtime::asio::thread_pool worker_pool_;
  std::optional<WorkGuard> daemon_work_;
  std::atomic<Runtime::Phase> phase_{Runtime::Phase::created};
  std::atomic<bool> stop_requested_{false};
  std::stop_source shutdown_stop_source_;
  runtime::JsEngine engine_;
  /**
   * Stable capabilities. Non-owning Impl references; valid for the full Impl
   * lifetime. Declared after engine so capability members are destroyed
   * before engine only in the reverse-order sense after FreeContext has
   * already completed while both were still alive.
   */
  Runtime::Async async_;
  Runtime::Callbacks callbacks_;
  Runtime::Script script_;
  bool job_pump_scheduled_{false};
  int fatal_exit_code_{0};

  static constexpr std::size_t kJobsPerTurn = 256;
};

}  // namespace vacps
