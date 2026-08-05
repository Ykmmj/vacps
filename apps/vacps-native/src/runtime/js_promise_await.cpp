#include "runtime/js_promise_await.hpp"

#include "qjs/scoped_cstring.hpp"
#include "runtime/detail/runtime_impl.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::runtime {
namespace detail {
namespace {

/** Terminal status for a single reverse-await operation (single-shot). */
enum class AwaitStatus : std::uint8_t {
  pending,
  fulfilled,
  rejected,
  timed_out,
  cancelled,
};

struct AwaitState;

/**
 * Concrete stop callback (no std::function allocation).
 *
 * May run on any thread. Holds only:
 *   - a copied owner executor for the post
 *   - weak_state → identity delivered to the owner-thread handler
 *
 * MUST NOT lock AwaitState on the foreign thread and MUST NOT read or mutate
 * status, timer, OwnedValue, stop callbacks, or any other AwaitState field
 * here. Status checks and the pending→cancelled transition run exclusively
 * on the owner executor after post. Runtime lifetime is a Narrow
 * precondition. Posting is noexcept and native allocation failure is fail-fast.
 *
 * stop_callback lifetime: never reset stop_callback from inside this
 * operator() or from the posted cancel lambda (std::stop_callback must not
 * destroy itself from its own callback). Owner-thread try_consume_terminal
 * resets both callbacks only after the notify has returned.
 */
struct AwaitCancelNotify {
  /** Copied owner executor — foreign notify never touches Impl fields. */
  asio::any_io_executor owner_ex;
  std::weak_ptr<AwaitState> weak_state;

  void operator()() const noexcept;  // defined after AwaitState
};

/**
 * Shared await state.
 *
 * Ownership:
 * - Retains the input thenable until the outer path consumes a terminal
 *   status (or timeout/cancel). Callbacks never reset input — JS_Call may
 *   still borrow it as this_val on a synchronous thenable settle.
 * - Holds the fulfillment JSValue until the coroutine consumes it (exactly
 *   once). Rejection / timeout / cancel are pure C++ Error on the settle path
 *   (no JS kept for those terminals).
 * - Does not own the callback function JSValues after attach. CallbackBox
 *   holds weak_ptr only (no State ↔ JS callback shared_ptr cycle; finalizer
 *   never destroys a state still holding live JSValues after FreeContext).
 * - Coroutine owns shared_ptr<AwaitState> for the wait; natural drain keeps
 *   the engine open until the waiter resumes and drops OwnedValues.
 * - One steady_timer: armed only while still pending (timeout, or max for no
 *   deadline); cancelled on async settle/stop to wake the waiter. Never used
 *   to poll Promise state. Sync terminal paths skip arm/wait.
 * - Optional stop_callbacks: caller_stop_cb (JsAwaitOptions::stop) and
 *   runtime_stop_cb (Impl::shutdown_stop_token). Installed only for a pending
 *   thenable when the corresponding token can stop. Destroyed only on the
 *   owner thread from try_consume_terminal — never from AwaitCancelNotify
 *   or its posted lambda (avoids stop_callback self-reset deadlock).
 * - All AwaitState fields are owner-thread-only after construction. Foreign
 *   stop delivery posts via AwaitCancelNotify's captured owner executor only.
 */
struct AwaitState {
  explicit AwaitState(asio::any_io_executor ex) : timer(std::move(ex)) {}

  AwaitState(const AwaitState&) = delete;
  AwaitState& operator=(const AwaitState&) = delete;

  AwaitStatus status{AwaitStatus::pending};
  vacps::qjs::OwnedValue input;
  vacps::qjs::OwnedValue fulfilled;
  Error rejected_error{};
  asio::steady_timer timer;
  std::optional<std::stop_callback<AwaitCancelNotify>> caller_stop_cb;
  std::optional<std::stop_callback<AwaitCancelNotify>> runtime_stop_cb;

  void cancel_timer() noexcept {
    // Failure is an internal executor/timer invariant violation. This helper
    // is noexcept intentionally; do not build a recovery state machine.
    (void)timer.cancel();
  }

  /**
   * Owner-thread cancel transition (pending → cancelled). Drops owned
   * JSValues, cancels the notification timer, single-shot. Does not destroy
   * stop callbacks (notify that posted this work must fully return first;
   * try_consume_terminal resets them on a later owner-thread step).
   */
  void apply_cancel_on_owner() noexcept {
    if (status != AwaitStatus::pending) {
      return;
    }
    input.reset();
    fulfilled.reset();
    status = AwaitStatus::cancelled;
    cancel_timer();
  }

  /** Owner-thread only: drop both stop callbacks after a terminal transition. */
  void reset_stop_callbacks() noexcept {
    caller_stop_cb.reset();
    runtime_stop_cb.reset();
  }

  void install_stop_callbacks(
      std::stop_token caller_stop,
      std::stop_token runtime_stop,
      const AwaitCancelNotify& notify) noexcept {
    if (caller_stop.stop_possible()) {
      caller_stop_cb.emplace(caller_stop, notify);
    }
    if (runtime_stop.stop_possible()) {
      runtime_stop_cb.emplace(runtime_stop, notify);
    }
  }
};

void AwaitCancelNotify::operator()() const noexcept {
  // Immutable routing only — no AwaitState field access off-owner.
  // Do not lock weak_state here; do not read status/timer/OwnedValue/stop cbs.
  asio::post(owner_ex, [weak = weak_state]() noexcept {
    // Owner thread only. Do not reset stop callbacks here: this handler may
    // have been posted by one of those callbacks.
    if (auto state = weak.lock()) {
      state->apply_cancel_on_owner();
    }
  });
}

/**
 * Native holder for JS_NewCFunctionData — finalizer deletes this only.
 *
 * Holds weak_ptr so CallbackBox finalization is never the last owner of an
 * AwaitState that might still carry live JSValues after FreeContext. The
 * coroutine (and temporary locks inside the callback) own the state; there
 * is no JS-callback ↔ state shared_ptr cycle.
 */
struct CallbackBox {
  std::weak_ptr<AwaitState> state;
  bool rejected{false};
};

std::mutex& callback_box_class_mu() noexcept {
  static std::mutex mu;
  return mu;
}

JSClassID& callback_box_class_id() noexcept {
  static JSClassID id = 0;
  return id;
}

void callback_box_finalizer(JSRuntime* /*rt*/, JSValue val) noexcept {
  // Finalizer contract: delete native holder only. No QuickJS calls, no
  // blocking, no C++ exceptions across the C boundary.
  const JSClassID id = callback_box_class_id();
  auto* box = static_cast<CallbackBox*>(JS_GetOpaque(val, id));
  JS_SetOpaque(val, nullptr);
  delete box;
}

[[nodiscard]] int ensure_callback_box_class(JSRuntime* rt) noexcept {
  JSClassID id = 0;
  {
    std::lock_guard<std::mutex> lock(callback_box_class_mu());
    auto& ref = callback_box_class_id();
    if (ref == 0) {
      JS_NewClassID(&ref);
    }
    id = ref;
  }
  if (JS_IsRegisteredClass(rt, id)) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(callback_box_class_mu());
    if (JS_IsRegisteredClass(rt, id)) {
      return 0;
    }
    JSClassDef def{};
    def.class_name = "vacps.JsAwaitCallback";
    def.finalizer = &callback_box_finalizer;
    return JS_NewClass(rt, id, &def);
  }
}

[[nodiscard]] Error error_from_js_value(
    JSContext* ctx,
    JSValueConst value) noexcept {
  auto text = vacps::qjs::ScopedCString::from_value(ctx, value);
  if (!text.empty()) {
    return Error::js(text.str());
  }
  // Secondary exception from JS_ToCString — clear so callers see a clean ctx.
  JS_FreeValue(ctx, JS_GetException(ctx));
  return Error::js("unknown JavaScript rejection");
}

[[nodiscard]] Error take_pending_js_error(
    JSContext* ctx,
    std::string_view fallback) noexcept {
  vacps::qjs::OwnedValue ex{ctx, JS_GetException(ctx)};
  if (ex.empty() || JS_IsNull(ex.get()) || JS_IsUndefined(ex.get())) {
    return Error::js(std::string{fallback});
  }
  return error_from_js_value(ctx, ex.get());
}

/**
 * Fulfill / reject reaction. Single-shot against AwaitState::status.
 * Late calls after timeout/cancel/settle are no-ops.
 *
 * Does NOT release state.input: JS_Call may still be using it as this_val on
 * a synchronous thenable path. The outer coroutine releases input only after
 * JS_Call returns (try_consume_terminal); timeout/cancel paths also release
 * on the JS owner thread.
 */
JSValue await_callback_entry(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv,
    int /*magic*/,
    JSValue* func_data) noexcept {
  auto* box = static_cast<CallbackBox*>(
      JS_GetOpaque(func_data[0], callback_box_class_id()));
  std::shared_ptr<AwaitState> pinned = box->state.lock();
  if (!pinned || pinned->status != AwaitStatus::pending) {
    return JS_UNDEFINED;
  }

  AwaitState& state = *pinned;
  const JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;

  if (box->rejected) {
    state.rejected_error = error_from_js_value(ctx, arg);
    state.status = AwaitStatus::rejected;
    state.fulfilled.reset();
    state.cancel_timer();
    return JS_UNDEFINED;
  }

  // Fulfill: retain a dup of the settlement value. Keep input alive.
  state.fulfilled = vacps::qjs::OwnedValue{ctx, JS_DupValue(ctx, arg)};
  state.status = AwaitStatus::fulfilled;
  state.cancel_timer();
  return JS_UNDEFINED;
}

/**
 * If state is already terminal, release retained input / stop callbacks and
 * produce the Result.
 *
 * Call only after JS_Call has returned (or on paths that never borrowed input
 * as this_val). Returns nullopt while still pending. Single place for terminal
 * consumption so callbacks never duplicate unsafe JSValue cleanup.
 *
 * Timeout → Error::native("JS await timed out").
 * Cancel  → Error::cancelled("JS await cancelled").
 */
[[nodiscard]] std::optional<Result<vacps::qjs::OwnedValue>> try_consume_terminal(
    AwaitState& state) noexcept {
  if (state.status == AwaitStatus::pending) {
    return std::nullopt;
  }
  // Safe: outer path owns the this_val lifetime past JS_Call.
  state.input.reset();
  state.reset_stop_callbacks();
  switch (state.status) {
    case AwaitStatus::fulfilled: {
      vacps::qjs::OwnedValue out = std::move(state.fulfilled);
      return Result<vacps::qjs::OwnedValue>{std::move(out)};
    }
    case AwaitStatus::rejected:
      state.fulfilled.reset();
      return std::unexpected(std::move(state.rejected_error));
    case AwaitStatus::timed_out:
      state.fulfilled.reset();
      return std::unexpected(Error::native("JS await timed out"));
    case AwaitStatus::cancelled:
      state.fulfilled.reset();
      return std::unexpected(Error::cancelled("JS await cancelled"));
    case AwaitStatus::pending:
      break;
  }
  std::unreachable();
}

/**
 * Build one native then-reaction callback.
 * On success, ownership of the function JSValue is returned to the caller.
 * Converts exactly one pending QuickJS exception into Error on failure.
 */
[[nodiscard]] Result<JSValue> create_await_callback(
    JSContext* ctx,
    const std::shared_ptr<AwaitState>& state,
    bool rejected) noexcept {
  if (ensure_callback_box_class(JS_GetRuntime(ctx)) < 0) {
    return std::unexpected(
        Error::js("JS await callback class registration failed"));
  }

  auto box = std::make_unique<CallbackBox>();
  box->state = std::weak_ptr<AwaitState>(state);
  box->rejected = rejected;

  JSValue holder =
      JS_NewObjectClass(ctx, static_cast<int>(callback_box_class_id()));
  if (JS_IsException(holder)) {
    return std::unexpected(take_pending_js_error(
        ctx, "JS await callback holder allocation failed"));
  }

  JS_SetOpaque(holder, box.release());

  JSValueConst data_c = holder;
  JSValue func = JS_NewCFunctionData(
      ctx,
      &await_callback_entry,
      /*length=*/1,
      /*magic=*/0,
      /*data_len=*/1,
      &data_c);

  // NewCFunctionData dups holder; drop our reference. On failure the holder
  // (and CallbackBox via finalizer) is freed here.
  JS_FreeValue(ctx, holder);

  if (JS_IsException(func)) {
    return std::unexpected(
        take_pending_js_error(ctx, "JS_NewCFunctionData failed"));
  }
  return func;
}

void free_value(JSContext* ctx, JSValue v) noexcept {
  JS_FreeValue(ctx, v);
}

[[nodiscard]] std::shared_ptr<AwaitState> make_await_state(
    asio::any_io_executor executor) noexcept {
  return std::make_shared<AwaitState>(std::move(executor));
}

}  // namespace

asio::awaitable<Result<vacps::qjs::OwnedValue>> JsPromiseAwaitAccess::await_value(
    vacps::Runtime& runtime,
    vacps::qjs::OwnedValue value,
    JsAwaitOptions options) {
  // Contract preconditions are established by the caller. This implementation
  // handles JavaScript and operational outcomes only; it does not revalidate
  // Runtime lifetime, owner thread, phase, context, or value ownership.
  vacps::Runtime::Impl& impl = *runtime.impl_;

  JSContext* ctx = impl.owner_context();
  const std::stop_token runtime_stop = impl.shutdown_stop_token();

  // Pending exception tag — extract once, never leave it on the context.
  if (value.is_exception()) {
    // Drop the exception-tag Value first; take_current_exception owns the
    // pending exception object via JS_GetException.
    value.reset();
    co_return std::unexpected(
        impl.engine().take_current_exception("await_value"));
  }

  // Pre-cancel: do not invoke arbitrary thenable code when either stop is
  // already requested. Non-thenable early returns below still honor this gate
  // so a cancelled caller never observes a late value from a raced stop.
  if (options.stop.stop_requested() || runtime_stop.stop_requested()) {
    value.reset();
    co_return std::unexpected(Error::cancelled("JS await cancelled"));
  }

  // Non-object → already settled value (return immediately).
  if (!value.is_object()) {
    co_return Result<vacps::qjs::OwnedValue>{std::move(value)};
  }

  // Bound arbitrary JS during thenable attachment (property lookup + then call).
  // Cleared before any coroutine suspension and on every early return.
  InterruptBudget budget{impl.engine(), impl.options().engine.js_time_budget};

  // thenable check: GetProperty "then". Preserve exact ownership / one exception.
  vacps::qjs::OwnedValue then =
      vacps::qjs::OwnedValue::get_property_str(ctx, value.get(), "then");
  if (then.is_exception()) {
    then.reset();
    budget.reset();
    co_return std::unexpected(
        impl.engine().take_current_exception("await_value then"));
  }
  if (!then.is_function()) {
    // Object without callable then — not a thenable; return as-is.
    then.reset();
    budget.reset();
    co_return Result<vacps::qjs::OwnedValue>{std::move(value)};
  }

  // ── Pending thenable path ────────────────────────────────────────────
  std::shared_ptr<AwaitState> state =
      make_await_state(impl.main_executor());

  state->input = std::move(value);

  // Install both stop callbacks before then-attach so a foreign-thread cancel
  // during JS_Call is posted and applied after the call returns (or wakes
  // the waiter). Capture owner executor at install so the foreign notify never
  // touches AwaitState fields — only posts via the copied executor.
  const AwaitCancelNotify notify{
      impl.main_executor(),
      std::weak_ptr<AwaitState>{state}};
  state->install_stop_callbacks(options.stop, runtime_stop, notify);

  // Race: stop requested between the early check and callback install, or a
  // posted cancel already applied. Do not call arbitrary thenable code.
  if (options.stop.stop_requested() || runtime_stop.stop_requested()) {
    state->apply_cancel_on_owner();
    then.reset();
    budget.reset();
    auto done = try_consume_terminal(*state);
    co_return std::move(*done);
  }

  auto on_fulfill = create_await_callback(ctx, state, /*rejected=*/false);
  if (!on_fulfill) {
    Error error = std::move(on_fulfill.error());
    then.reset();
    state->input.reset();
    state->reset_stop_callbacks();
    budget.reset();
    co_return std::unexpected(std::move(error));
  }

  auto on_reject = create_await_callback(ctx, state, /*rejected=*/true);
  if (!on_reject) {
    Error error = std::move(on_reject.error());
    free_value(ctx, *on_fulfill);
    then.reset();
    state->input.reset();
    state->reset_stop_callbacks();
    budget.reset();
    co_return std::unexpected(std::move(error));
  }

  // Call then(onFulfilled, onRejected). thenable may invoke callbacks sync.
  // state->input.get() is this_val for the duration of JS_Call — callbacks
  // must not reset input while that borrow is live.
  JSValueConst argv[2] = {*on_fulfill, *on_reject};
  JSValue call_ret =
      JS_Call(ctx, then.get(), state->input.get(), 2, argv);

  // Drop our refs to the callback functions after attach. The Promise/thenable
  // retains reactions; State must not own the function JSValues (cycle risk).
  free_value(ctx, *on_fulfill);
  free_value(ctx, *on_reject);
  then.reset();

  if (JS_IsException(call_ret)) {
    // Always extract/clear the pending exception so the context stays clean.
    // Native allocation failure propagates to the fail-fast coroutine
    // completion boundary; it is not repackaged as a second JS error.
    Error err =
        impl.engine().take_current_exception("await_value then call");
    free_value(ctx, call_ret);
    // If a sync callback already settled, prefer that terminal status.
    if (state->status == AwaitStatus::pending) {
      state->rejected_error = std::move(err);
      state->status = AwaitStatus::rejected;
      state->fulfilled.reset();
      state->cancel_timer();
      budget.reset();
      auto done = try_consume_terminal(*state);
      co_return std::move(*done);
    }
    // Already settled by a synchronous reaction — settlement wins; exception
    // has been cleared above. Fall through to try_consume_terminal.
  } else {
    free_value(ctx, call_ret);
  }

  // JS attach path finished — clear interrupt before any further work/suspend.
  budget.reset();

  // Synchronous thenable settlement or cancel applied during attach.
  if (auto done = try_consume_terminal(*state)) {
    co_return std::move(*done);
  }

  // Ordinary Promise: reactions run as QuickJS jobs. Schedule the pump, then
  // install a wait that settlement/cancel can cancel. No polling.
  // A posted pump cannot settle inline before this coroutine suspends.
  // Natural drain keeps the engine live for in-flight reverse awaits — no
  // phase/engine probes around the pump schedule.
  impl.schedule_job_pump();

  if (auto done = try_consume_terminal(*state)) {
    co_return std::move(*done);
  }

  // Arm the single notification/timeout timer only while still pending.
  // Timer service failure is an internal executor invariant violation; this
  // coroutine does not construct an alternate recovery state machine.
  if (options.timeout.count() > 0) {
    state->timer.expires_after(options.timeout);
  } else {
    // No deadline: wait until settle/cancel cancels the timer.
    state->timer.expires_at(asio::steady_timer::time_point::max());
  }

  // Re-check after arm: never async_wait once terminal. A cancel issued
  // before async_wait is installed does not complete a future wait.
  if (auto done = try_consume_terminal(*state)) {
    co_return std::move(*done);
  }

  auto [ec] = co_await state->timer.async_wait(
      asio::as_tuple);

  // ── After resume: interpret single-shot status ───────────────────────
  // Prefer terminal already set by settle/cancel (those mark status before
  // cancelling the timer). Do not discard ec while still pending.
  if (auto done = try_consume_terminal(*state)) {
    co_return std::move(*done);
  }

  if (state->status == AwaitStatus::pending) {
    if (!ec) {
      // Successful timer expiry → timeout (distinct from cancellation).
      state->status = AwaitStatus::timed_out;
      state->fulfilled.reset();
    } else if (ec == asio::error::operation_aborted) {
      // External cancellation without a terminal transition (e.g. bare timer
      // cancel). Map to cancelled, not timeout.
      state->input.reset();
      state->fulfilled.reset();
      state->reset_stop_callbacks();
      co_return std::unexpected(Error::cancelled("JS await cancelled"));
    } else {
      // Any other wait error is a native/internal failure, not timeout.
      state->input.reset();
      state->fulfilled.reset();
      state->reset_stop_callbacks();
      co_return std::unexpected(Error::native(
          std::string{"JS await timer wait failed: "} + ec.message()));
    }
  }

  auto done = try_consume_terminal(*state);
  co_return std::move(*done);
}

}  // namespace detail
}  // namespace vacps::runtime
