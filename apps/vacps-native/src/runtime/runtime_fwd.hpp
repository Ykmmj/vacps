#pragma once

/**
 * Canonical vacps::Runtime declaration and nested public types.
 *
 * Include this when only Runtime / Runtime::Async* / Options are needed.
 * Full capability definitions live in runtime.hpp (via the capability headers).
 *
 * Runtime owns one unique Runtime::Impl (internal implementation). There is no
 * shared whole-runtime state object and no product core()/state() accessor.
 * JS_SetContextOpaque stores the stable public Runtime* when the slot is retained.
 *
 * Contract: Narrow (full Runtime lifetime) — see RUNTIME_LAYERING.md /
 * NATIVE_RESOURCE_OWNERSHIP.md. Runtime outlives run() and every capability
 * reference; after initialize(), destroy on the owner thread.
 *
 * Normal shutdown is Asio natural drain: request_stop cooperatively cancels
 * via one runtime-wide stop_source, releases only the daemon work_guard, and
 * leaves main_io_.run() active until outstanding work completes. QuickJS stays
 * open for the entire drain; run closes it after drain and before returning.
 */

#include "runtime/error.hpp"
#include "runtime/js_engine.hpp"
#include "runtime/js_promise_await.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace vacps {

namespace runtime {
namespace asio = boost::asio;
}  // namespace runtime

/**
 * Public Runtime facade (Asio + QuickJS host).
 *
 * Minimal host surface:
 * initialize → run(startup) → request_stop
 * async() / callbacks() / script() return stable capability references
 * owned by Runtime::Impl. post_to_owner / context / evaluate / await_value and the main
 * executor complete the public host surface.
 *
 * Lifecycle: created → initialized → running → stopping → closed.
 * Once initialize() establishes a JS owner, Runtime must be destroyed on
 * that owner thread (including after close). Never-initialized may go anywhere.
 *
 * Owner/context/lifecycle invariants are established at true Runtime entry
 * points (initialize, run, evaluate, await_value, Script/Callbacks entry). Once
 * an owner QuickJS / main_io turn is established, helpers do not re-package
 * the same affinity as recoverable Result errors.
 *
 * Does not expose main_io, engine internals, worker_pool, or a core()/state()
 * accessor. Impl is never shared. Does not call io_context::stop() on the
 * normal shutdown path.
 */
class Runtime {
 public:
  struct Options {
    std::size_t worker_threads{4};
    runtime::EngineOptions engine{};
  };

  /** JS → Promise / run_blocking. */
  class Async;
  /** Native event → JS function → await sync/thenable. */
  class Callbacks;
  /** Owner-thread module eval / export invoke. */
  class Script;

  explicit Runtime(Options options);
  ~Runtime() noexcept;

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  /**
   * Contract: Narrow — phase == created; called once.
   * Errors: Result for expected engine/allocation failure only.
   */
  [[nodiscard]] runtime::VoidResult initialize();

  /**
   * Drive main io_context until natural drain after stop. Current thread is
   * the sole JS thread. Startup runs only after phase becomes running.
   *
   * Contract: Narrow — initialized; same thread as initialize; called once.
   * Preconditions: startup contains a callable target.
   * @return 0 on success / normal request_stop; non-zero on fatal operational
   * startup, main-loop, or post-drain failure (still closes cleanly).
   * Programmer misuse of preconditions is not a product integer failure mode.
   */
  int run(std::move_only_function<runtime::VoidResult() noexcept> startup);

  /** Any thread; idempotent. Must not race Runtime destruction. */
  void request_stop() noexcept;

  /**
   * Post work onto the JS / main_io thread.
   * Safe from any thread while Runtime is alive.
   * Posted callable must not throw (owner handler is noexcept).
   */
  [[nodiscard]] runtime::VoidResult post_to_owner(
      std::move_only_function<void() noexcept> f);

  /** Stable capabilities owned by Impl (valid for Runtime lifetime). */
  [[nodiscard]] Async& async() noexcept;
  [[nodiscard]] const Async& async() const noexcept;
  [[nodiscard]] Callbacks& callbacks() noexcept;
  [[nodiscard]] const Callbacks& callbacks() const noexcept;
  [[nodiscard]] Script& script() noexcept;
  [[nodiscard]] const Script& script() const noexcept;

  /**
   * Live JSContext on the JS owner thread while the engine is open.
   *
   * Contract: Narrow
   * Preconditions: owner thread; engine open; phase is
   * initialized | running | stopping.
   * No off-owner / post-close nullptr recovery is part of the product contract.
   */
  [[nodiscard]] JSContext* context() noexcept;

  /**
   * Evaluate source on the JS owner thread (interrupt budget + post job pump).
   *
   * Contract: Narrow
   * Preconditions: owner thread; engine open; phase is
   * initialized | running | stopping.
   * Errors: JavaScript evaluation / job-scheduling failures only — not
   * wrong-thread or wrong-phase misuse.
   */
  [[nodiscard]] runtime::Result<vacps::qjs::OwnedValue> evaluate(
      std::string_view source,
      std::string_view filename,
      int flags);

  /**
   * Await a JS Promise/thenable as an Asio awaitable (event-driven).
   *
   * Contract: Narrow
   * Preconditions: owner thread; phase==running; engine open; matching live
   * OwnedValue (non-empty, same owner context). These are unchecked caller
   * preconditions; violations are not Error::invalid_state recovery paths.
   *
   * Operational results: JavaScript rejection, timeout, cancellation, and
   * QuickJS API failures remain modeled Results. Runtime shutdown cancels
   * pending reverse awaits via the runtime stop source (Error::cancelled).
   * Non-object or object-without-callable-then values complete immediately.
   * Promise/thenable values attach native fulfill/reject reactions — never
   * polls JS_PromiseState. options.timeout <= 0 means no deadline.
   * options.stop may cancel a pending thenable (Error::cancelled); pre-cancel
   * skips arbitrary thenable code. Timeout and cancellation are distinct.
   */
  [[nodiscard]] runtime::asio::awaitable<
      runtime::Result<vacps::qjs::OwnedValue>>
  await_value(
      vacps::qjs::OwnedValue value,
      runtime::JsAwaitOptions options = {});

  [[nodiscard]] runtime::asio::any_io_executor main_executor() noexcept;

 private:
  friend struct runtime::detail::JsPromiseAwaitAccess;

  enum class Phase {
    created,
    initialized,
    running,
    stopping,
    closed,
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vacps
