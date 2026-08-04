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
 * Synchronous native setup exceptions (task-state allocation, callable
 * materialization, co_spawn initiation) are not recoverable Promise
 * failures: promise() / promise_void() are noexcept and fail fast.
 *
 * Ownership:
 *   - Runtime::Async is Impl-owned. Holds a non-owning Runtime::Impl*.
 *   - PromiseTaskState is operation-local shared ownership (coroutine frame
 *     and co_spawn completion handler share the capability).
 *   - Handlers capture non-owning Async pointers plus shared_ptr to
 *     operation state; never shared ownership of the whole Runtime.
 *   - Managed Start receives Impl's runtime-wide shutdown stop_token by
 *     value (cooperative cancellation under natural drain).
 *
 * Error paths:
 *   - JS_NewPromiseCapability failure → return JS_EXCEPTION unchanged.
 *   - Result errors from Start/Encode and successful resolve each settle
 *     once on the owner thread, then schedule the job pump exactly once.
 *     Pump scheduling is explicit on those paths (no destructor/scope-exit
 *     pump) so coroutine exception unwinding cannot double-schedule.
 *   - Uncaught Start/Encode exceptions after the coroutine is armed →
 *     co_spawn completion (exception_ptr) maps once via
 *     error_from_exception_ptr and rejects once, then schedules the pump
 *     exactly once. Native std::bad_alloc on that path terminates.
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
#include <boost/asio/use_awaitable.hpp>

#include <quickjs.h>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

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

template <class T, PromiseStart Start>
Task<T> invoke_start(Start& start, std::stop_token stop) {
  if constexpr (std::invocable<Start&, std::stop_token>) {
    co_return co_await std::invoke(start, stop);
  } else {
    co_return co_await std::invoke(start);
  }
}

/**
 * Named coroutine helper for promise_void — not an IIFE coroutine lambda.
 * Adapts Start → Task<void> into Task<monostate> for promise<monostate>.
 */
template <class Start>
Task<std::monostate> promise_void_task(Start start, std::stop_token stop) {
  Result<void> result;
  if constexpr (std::invocable<Start&, std::stop_token>) {
    result = co_await std::invoke(start, stop);
  } else {
    (void)stop;
    result = co_await std::invoke(start);
  }
  if (!result) {
    co_return std::unexpected(std::move(result.error()));
  }
  co_return std::monostate{};
}

/** Non-coroutine start object; preserves move-only Start support. */
template <class Start>
struct promise_void_start {
  Start start;

  Task<std::monostate> operator()(std::stop_token stop) {
    return promise_void_task(std::move(start), std::move(stop));
  }
};

/**
 * One native Promise task. Shared only so the coroutine body and the
 * co_spawn completion handler can both reach the capability. The engine
 * outlives this state under Runtime natural drain.
 */
struct PromiseTaskState {
  explicit PromiseTaskState(PromiseCapability capability) noexcept
      : capability(std::move(capability)) {}

  PromiseCapability capability;
};

}  // namespace detail

}  // namespace vacps::runtime

namespace vacps {

using runtime::Task;
using runtime::PromiseStart;

class Runtime::Async {
 public:
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

  // Direct construction: setup allocation failure is not a recoverable
  // Promise rejection (noexcept boundary → terminate on unexpected throw).
  auto task_state = std::make_shared<runtime::detail::PromiseTaskState>(
      std::move(capability));

  // Capture the runtime-wide shutdown token by value for cooperative Start
  // cancellation. Natural drain keeps the engine live until this operation
  // and its completion handler finish.
  std::stop_token stop = shutdown_stop_token();

  using StartType = std::decay_t<Start>;
  using EncodeType = std::decay_t<Encode>;

  // Materialize Start/Encode exactly once into the coroutine frame via the
  // constrained forwarding construction. No broad setup try/catch: expected
  // domain failures use Result; post-launch exceptions use co_spawn's
  // exception_ptr completion path; native setup throws fail fast.
  runtime::asio::co_spawn(
      executor(),
      [self = this,
       task_state,
       stop = std::move(stop),
       start = StartType(std::forward<Start>(start)),
       encode = EncodeType(std::forward<Encode>(encode))]() mutable
      -> runtime::asio::awaitable<void> {
        runtime::Result<T> result =
            co_await runtime::detail::invoke_start<T>(start, std::move(stop));

        if (!result) {
          self->settle_or_report(task_state->capability, result.error());
          self->schedule_job_pump();
          co_return;
        }

        runtime::Result<vacps::qjs::OwnedValue> encoded = std::invoke(
            encode, self->owner_context(), std::move(*result));

        if (!encoded) {
          self->settle_or_report(task_state->capability, encoded.error());
          self->schedule_job_pump();
          co_return;
        }
        auto resolved = task_state->capability.resolve(encoded->get());
        if (!resolved) {
          self->report_error(resolved.error());
        }
        self->schedule_job_pump();
      },
      [self = this, task_state](std::exception_ptr ep) noexcept {
        if (ep == nullptr) {
          return;
        }
        // Exception completion path: reject once and schedule the pump once.
        // Native std::bad_alloc terminates inside error_from_exception_ptr.
        const runtime::Error err = runtime::error_from_exception_ptr(
            std::move(ep),
            runtime::Errc::native_failure,
            "unknown exception in async task");
        self->settle_or_report(task_state->capability, err);
        self->schedule_job_pump();
      });
  return holder.release();
}

template <PromiseStart Start>
  requires runtime::detail::PromiseArgMaterialize<Start>
JSValue Runtime::Async::promise_void(JSContext* ctx, Start&& start) noexcept {
  using StartType = std::decay_t<Start>;
  return promise<std::monostate>(
      ctx,
      runtime::detail::promise_void_start<StartType>{std::forward<Start>(start)},
      [](JSContext* c, std::monostate) -> runtime::Result<vacps::qjs::OwnedValue> {
        return vacps::qjs::OwnedValue{c, JS_DupValue(c, JS_UNDEFINED)};
      });
}

template <runtime::BlockingCallable Fn>
auto Runtime::Async::run_blocking(std::stop_token stop, Fn&& fn) {
  using Function = std::decay_t<Fn>;
  static_assert(
      !runtime::detail::is_js_thread_confined_v<
          runtime::detail::worker_value_result_t<Function>>,
      "Runtime::Async::run_blocking must not return JS-thread-confined types");
  // No whole-Runtime lifetime pin. Frame owns callable + BlockingWorkerState.
  return runtime::detail::run_blocking_coro<Function>(
      worker_executor(),
      stop,
      Function(std::forward<Fn>(fn)));
}

}  // namespace vacps
