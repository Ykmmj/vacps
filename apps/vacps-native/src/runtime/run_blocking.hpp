#pragma once

/**
 * Thread-pool blocking work via Boost.Asio native post.
 *
 * Used only through Runtime::Async::run_blocking. The JS surface does not
 * expose a generic runBlocking API.
 *
 * Contract: Narrow
 * Preconditions:
 *   - Invoked from a coroutine on the Runtime owner (JS) executor while the
 *     Runtime remains alive (Runtime lifetime contract).
 *   - Fn is pure C++: must not capture JSContext*, JSValue, qjs::OwnedValue,
 *     ScopedCString, PromiseCapability, or other JS-owning RAII.
 * Errors:
 *   - Result error for expected domain/I/O failure returned by Fn
 *   - Error::cancelled when stop is requested before start or before the
 *     owner consumes the completed result
 *   - Error::native when Fn throws (mapped from exception_ptr on owner resume)
 * Threading:
 *   - Fn runs on the worker pool; completion resumes on the coroutine's
 *     associated executor (owner/main_io).
 * Lifetime:
 *   - The posted function owns Fn and returns a typed C++ outcome through
 *     Asio's non-void post completion. The coroutine consumes that outcome
 *     after resuming on its associated owner executor.
 *   - No whole-Runtime shared_ptr / keep_alive pin.
 *
 * Cancellation (honest):
 *   - A queued individual post cannot be removed by Asio. The noexcept
 *     wrapper skips the callable when dequeued if stop is already requested.
 *   - A running blocking callable is only cooperatively cancellable when it
 *     accepts std::stop_token. Arbitrary blocking C++ cannot be preempted.
 *   - After work, stop is checked once before consuming the result. Completion
 *     wins over cancellation requested after that owner-side decision.
 *
 * Exceptions:
 *   - Boost.Asio states that a function submitted to thread_pool that throws
 *     reaches the target executor and thread_pool calls std::terminate.
 *     Therefore the exact lambda posted to the pool is noexcept and stores
 *     std::current_exception() as data; message strings are built only after
 *     resume on the owner thread.
 */

#include "runtime/error.hpp"
#include "runtime/run_blocking_traits.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>

#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace vacps::runtime {

namespace asio = boost::asio;

namespace detail {

/**
 * Pure-C++ result transported from the worker function to the coroutine's
 * associated owner executor by Asio's non-void post completion.
 */
template <class Value>
struct BlockingWorkerOutcome {
  std::exception_ptr exception{};
  std::optional<Result<Value>> result{};
};

/**
 * Named coroutine helper — avoid IIFE coroutine (ASan stack-use-after-return).
 *
 * Posts a noexcept wrapper to worker_executor; completion resumes on the
 * caller's associated executor via Asio's deferred direct-await path.
 *
 * The posted function owns its callable and stop token. Its typed outcome is
 * moved back to the coroutine without operation-local shared allocation or a
 * whole-Runtime lifetime pin.
 */
template <class Function>
asio::awaitable<Result<worker_value_result_t<Function>>> run_blocking_coro(
    asio::any_io_executor worker_executor,
    std::stop_token stop,
    Function fn) {
  using Value = worker_value_result_t<Function>;
  static_assert(
      !is_js_thread_confined_v<Value>,
      "run_blocking worker must not return JS-thread-confined types");

  if (stop.stop_requested()) {
    co_return std::unexpected(Error::cancelled());
  }

  // Exact lambda submitted to thread_pool: must be noexcept so exceptions
  // never reach the pool (which would std::terminate). Non-void post moves the
  // returned outcome to the caller coroutine's associated owner executor.
  BlockingWorkerOutcome<Value> outcome = co_await asio::post(
      [fn = std::move(fn), stop]() mutable noexcept
      -> BlockingWorkerOutcome<Value> {
        BlockingWorkerOutcome<Value> worker_outcome;
        if (stop.stop_requested()) {
          return worker_outcome;
        }
        try {
          worker_outcome.result.emplace(invoke_blocking(fn, stop));
        } catch (...) {
          worker_outcome.exception = std::current_exception();
        }
        return worker_outcome;
      },
      worker_executor);

  // Owner / associated executor resumes here.
  if (stop.stop_requested()) {
    co_return std::unexpected(Error::cancelled());
  }
  if (outcome.exception != nullptr) {
    co_return std::unexpected(error_from_exception_ptr(
        std::move(outcome.exception),
        Errc::native_failure,
        "unknown exception in run_blocking"));
  }
  co_return std::move(*outcome.result);
}

}  // namespace detail
}  // namespace vacps::runtime
