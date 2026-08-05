#pragma once

/**
 * Runtime::Async — sole C++→JS Promise creation/settlement entry and
 * public run_blocking (worker-pool pure-C++ work).
 *
 * Contract: Narrow (promise / promise_void / run_blocking)
 * Preconditions:
 *   - Invoked from a live QuickJS owner-thread callback/turn with the
 *     matching live JSContext while the Runtime is accepting normal JS work.
 *   - Start / Encode obey documented ownership rules: do NOT capture
 *     JSContext*, JSValue, vacps::qjs::OwnedValue, PromiseCapability, or
 *     other JS-owning RAII across co_await / worker suspension.
 *   - Start / Encode can be materialized via the exact forwarding
 *     construction decay_t<Arg>(Arg&&) without throwing, and the decayed
 *     type is nothrow-move-constructible (co_spawn closure move). Enforced
 *     by PromiseArgMaterialize.
 *   - run_blocking Fn is pure C++ only (see run_blocking.hpp).
 *   - Owning Runtime/Impl outlives every use (Runtime lifetime contract).
 * Violations are programmer errors with no recovery guarantee. Preconditions
 * are caller obligations and are not dynamically rechecked; there is no
 * recoverable JS InternalError / Result path for wrong thread, wrong context,
 * use-after-Runtime, or phase.
 * Synchronous native setup exceptions (coroutine/handler allocation, callable
 * materialization, co_spawn initiation) are not recoverable Promise
 * failures: promise() / promise_void() are noexcept and fail fast.
 *
 * Ownership:
 *   - Runtime::Async is Impl-owned. Holds a non-owning Runtime::Impl*.
 *   - The coroutine frame owns Start / Encode and returns only
 *     Result<qjs::OwnedValue>. The co_spawn completion handler exclusively
 *     owns PromiseCapability and performs the single settlement.
 *   - Coroutine and handler capture a non-owning Async pointer under the
 *     Runtime lifetime contract; never shared ownership of the whole Runtime.
 *   - Managed Start receives Impl's runtime-wide shutdown stop_token by
 *     value (cooperative cancellation under natural drain).
 *
 * Error paths:
 *   - JS_NewPromiseCapability failure → return JS_EXCEPTION unchanged.
 *   - Start/Encode Result errors and successful encoded values are returned
 *     to the co_spawn completion handler, which settles once and schedules
 *     the job pump once.
 *   - Uncaught Start/Encode exceptions after the coroutine is armed arrive at
 *     the same completion handler as exception_ptr and are mapped once via
 *     error_from_exception_ptr. Native std::bad_alloc terminates.
 *   - Natural drain keeps QuickJS open until every co_spawn operation and
 *     its completion handler finish; settlement does not probe phase,
 *     engine-open, or capability-alive state.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"
#include "runtime/js_encode.hpp"
#include "runtime/run_blocking.hpp"
#include "runtime/run_blocking_traits.hpp"
#include "runtime/promise_capability.hpp"
#include "runtime/runtime_fwd.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>

#include <quickjs.h>

#include <concepts>
#include <exception>
#include <functional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace vacps::runtime {

namespace asio = boost::asio;

template <class T>
using Task = asio::awaitable<Result<T>>;

/** Start callable for promise: () or (stop_token) → awaitable Result-ish. */
template <class Start>
concept PromiseStart =
    std::invocable<std::decay_t<Start>&, std::stop_token> ||
    std::invocable<std::decay_t<Start>&>;

namespace detail {

/**
 * Forward Promise Start/Encode materialization contract: the exact
 * forwarding construction decay_t<Arg>(Arg&&) is non-throwing, and the
 * decayed object can later be moved into the co_spawn closure without
 * throwing. Local to this header — not a generic traits framework.
 */
template <class Arg>
concept PromiseArgMaterialize =
    std::is_nothrow_constructible_v<std::decay_t<Arg>, Arg&&> &&
    std::is_nothrow_move_constructible_v<std::decay_t<Arg>>;

}  // namespace detail

}  // namespace vacps::runtime

namespace vacps {

using runtime::Task;
using runtime::PromiseStart;

class Runtime::Async {
 public:
  /**
   * Per-resource serialized worker lane.
   *
   * Copies retain the identity of the same Asio strand. The lane does not own
   * the Runtime worker pool; Runtime must outlive every submitted operation.
   */
  class SerialWorker {
   public:
    SerialWorker(const SerialWorker&) = default;
    SerialWorker& operator=(const SerialWorker&) = default;
    SerialWorker(SerialWorker&&) noexcept = default;
    SerialWorker& operator=(SerialWorker&&) noexcept = default;

   private:
    friend class Async;

    explicit SerialWorker(runtime::asio::any_io_executor executor) noexcept
        : executor_(std::move(executor)) {}

    runtime::asio::any_io_executor executor_;
  };

  /**
   * Non-owning reference to the owning Runtime::Impl. Valid for the full Runtime
   * lifetime under the documented Narrow contract.
   */
  explicit Async(Impl& impl) noexcept;

  Async(const Async&) = delete;
  Async& operator=(const Async&) = delete;
  Async(Async&&) = delete;
  Async& operator=(Async&&) = delete;

  /**
   * Create a JS Promise bound to a managed native task.
   *
   * Contract: Narrow — see file header. Start must not park JS-owning state
   * across suspension. Encode runs on the JS thread after Start completes.
   * Start/Encode materialization (forward construction + subsequent move
   * into the co_spawn closure) is a compile-time non-throwing contract.
   * Synchronous native setup exceptions are not recoverable Promise
   * failures (this function is noexcept).
   */
  template <class T, PromiseStart Start, class Encode>
    requires runtime::JsEncode<Encode, T> &&
             runtime::detail::PromiseArgMaterialize<Start> &&
             runtime::detail::PromiseArgMaterialize<Encode>
  [[nodiscard]] JSValue promise(JSContext* ctx, Start&& start, Encode&& encode) noexcept;

  /**
   * Void-result overload of promise(). Same Narrow contract; Start must
   * satisfy PromiseArgMaterialize. noexcept: setup exceptions fail fast.
   */
  template <PromiseStart Start>
    requires runtime::detail::PromiseArgMaterialize<Start>
  [[nodiscard]] JSValue promise_void(JSContext* ctx, Start&& start) noexcept;

  /**
   * Run pure C++ work on the worker pool; resume on the owner executor.
   *
   * Contract: Narrow — see run_blocking.hpp. No JS-thread-confined captures
   * or return types. Cancellation is cooperative (queued skip / stop_token).
   */
  template <runtime::BlockingCallable Fn>
  [[nodiscard]] auto run_blocking(std::stop_token stop, Fn&& fn);

  /**
   * Create an independent FIFO lane over the Runtime worker pool.
   *
   * Contract: Narrow
   * Preconditions: owning Runtime outlives the lane and all work submitted
   *                 through it
   * Errors: allocation/setup exceptions are not operational Result failures
   * Threading: create and submit from the Runtime owner executor
   * Lifetime: returned lane is non-owning with respect to Runtime
   */
  [[nodiscard]] SerialWorker make_serial_worker() const;

  /**
   * Run pure C++ work on a per-resource FIFO worker lane.
   *
   * Contract: Narrow — the lane came from this live Runtime::Async; Fn obeys
   * the same worker ownership and cancellation contract as run_blocking().
   */
  template <runtime::BlockingCallable Fn>
  [[nodiscard]] auto run_blocking(
      const SerialWorker& worker,
      std::stop_token stop,
      Fn&& fn);

  [[nodiscard]] runtime::asio::any_io_executor executor() const;

 private:
  // Non-template bridges keep Impl out of this public header body.
  // Defined in runtime_async.cpp (includes internal Impl definition).
  [[nodiscard]] JSContext* owner_context() const noexcept;
  [[nodiscard]] std::stop_token shutdown_stop_token() const noexcept;
  [[nodiscard]] runtime::asio::any_io_executor worker_executor() const;
  void schedule_job_pump() noexcept;
  void report_error(const runtime::Error& error) noexcept;
  void settle_or_report(
      runtime::PromiseCapability& capability,
      runtime::Error err) noexcept;
  void complete_promise(
      runtime::PromiseCapability& capability,
      std::exception_ptr ep,
      runtime::Result<vacps::qjs::OwnedValue> result) noexcept;
  void complete_void_promise(
      runtime::PromiseCapability& capability,
      std::exception_ptr ep,
      runtime::VoidResult result) noexcept;

  Impl& impl_;
};

template <class T, PromiseStart Start, class Encode>
  requires runtime::JsEncode<Encode, T> &&
           runtime::detail::PromiseArgMaterialize<Start> &&
           runtime::detail::PromiseArgMaterialize<Encode>
JSValue Runtime::Async::promise(JSContext* ctx, Start&& start, Encode&& encode) noexcept {
  JSContext* owner_ctx = ctx;

  // --- Promise + capability under RAII before any throwing C++ work. ---
  JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue raw_promise = JS_NewPromiseCapability(owner_ctx, resolving);
  if (JS_IsException(raw_promise)) {
    return raw_promise;
  }
  vacps::qjs::OwnedValue holder{owner_ctx, raw_promise};
  runtime::PromiseCapability capability{owner_ctx, resolving[0], resolving[1]};

  // Capture the runtime-wide shutdown token by value for cooperative Start
  // cancellation. Natural drain keeps the engine live until this operation
  // and its completion handler finish.
  std::stop_token stop = shutdown_stop_token();

  using StartType = std::decay_t<Start>;
  using EncodeType = std::decay_t<Encode>;

  // Materialize Start/Encode exactly once into the coroutine frame via the
  // constrained forwarding construction. No broad setup try/catch: expected
  // domain failures are returned to the completion handler; post-launch
  // exceptions use co_spawn's exception_ptr path; native setup throws fail
  // fast.
  runtime::asio::co_spawn(
      executor(),
      [self = this,
       stop = std::move(stop),
       start = StartType(std::forward<Start>(start)),
       encode = EncodeType(std::forward<Encode>(encode))]() mutable
      -> runtime::Task<vacps::qjs::OwnedValue> {
        // Select the Start signature without another forwarding coroutine
        // frame. This local lambda is ordinary (not a coroutine): it only
        // materializes the awaitable returned by Start.
        auto operation = [&]() {
          if constexpr (std::invocable<StartType&, std::stop_token>) {
            return std::invoke(start, std::move(stop));
          } else {
            return std::invoke(start);
          }
        }();
        runtime::Result<T> result = co_await std::move(operation);

        if (!result) {
          co_return std::unexpected(std::move(result.error()));
        }

        co_return std::invoke(
            encode, self->owner_context(), std::move(*result));
      },
      [self = this, capability = std::move(capability)](
          std::exception_ptr ep,
          runtime::Result<vacps::qjs::OwnedValue> result) mutable noexcept {
        self->complete_promise(
            capability, std::move(ep), std::move(result));
      });
  return holder.release();
}

template <PromiseStart Start>
  requires runtime::detail::PromiseArgMaterialize<Start>
JSValue Runtime::Async::promise_void(JSContext* ctx, Start&& start) noexcept {
  using StartType = std::decay_t<Start>;
  JSContext* owner_ctx = ctx;

  JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue raw_promise = JS_NewPromiseCapability(owner_ctx, resolving);
  if (JS_IsException(raw_promise)) {
    return raw_promise;
  }
  vacps::qjs::OwnedValue holder{owner_ctx, raw_promise};
  runtime::PromiseCapability capability{owner_ctx, resolving[0], resolving[1]};
  std::stop_token stop = shutdown_stop_token();

  runtime::asio::co_spawn(
      executor(),
      [stop = std::move(stop),
       start = StartType(std::forward<Start>(start))]() mutable
      -> runtime::Task<void> {
        auto operation = [&]() {
          if constexpr (std::invocable<StartType&, std::stop_token>) {
            return std::invoke(start, std::move(stop));
          } else {
            return std::invoke(start);
          }
        }();
        runtime::VoidResult result = co_await std::move(operation);
        if (!result) {
          co_return std::unexpected(std::move(result.error()));
        }
        co_return runtime::success();
      },
      [self = this, capability = std::move(capability)](
          std::exception_ptr ep,
          runtime::VoidResult result) mutable noexcept {
        self->complete_void_promise(
            capability, std::move(ep), std::move(result));
      });
  return holder.release();
}

template <runtime::BlockingCallable Fn>
auto Runtime::Async::run_blocking(std::stop_token stop, Fn&& fn) {
  using Function = std::decay_t<Fn>;
  static_assert(
      !runtime::detail::is_js_thread_confined_v<
          runtime::detail::worker_value_result_t<Function>>,
      "Runtime::Async::run_blocking must not return JS-thread-confined types");
  // No whole-Runtime lifetime pin. Asio transports the typed worker outcome
  // back to this coroutine's associated owner executor.
  return runtime::detail::run_blocking_coro<Function>(
      worker_executor(),
      stop,
      Function(std::forward<Fn>(fn)));
}

template <runtime::BlockingCallable Fn>
auto Runtime::Async::run_blocking(
    const SerialWorker& worker,
    std::stop_token stop,
    Fn&& fn) {
  using Function = std::decay_t<Fn>;
  static_assert(
      !runtime::detail::is_js_thread_confined_v<
          runtime::detail::worker_value_result_t<Function>>,
      "Runtime::Async::run_blocking must not return JS-thread-confined types");
  return runtime::detail::run_blocking_coro<Function>(
      worker.executor_,
      stop,
      Function(std::forward<Fn>(fn)));
}

}  // namespace vacps
