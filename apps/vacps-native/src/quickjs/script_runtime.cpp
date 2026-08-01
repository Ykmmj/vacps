#include "quickjs/script_runtime.hpp"

#include "quickjs/js_bridge.hpp"
#include "app/log.hpp"
#include "app/version.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fstream>
#include <format>
#include <iterator>
#include <utility>

namespace vacps::js {

Result<std::string> read_script_file(std::string_view path) {
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    return std::unexpected(Error{std::format("cannot read script: {}", path)});
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ScriptRuntime::ScriptRuntime(
    Runtime runtime,
    Context context,
    asio::io_context& ioc,
    EngineOptions engine,
    std::shared_ptr<ScriptServices> services)
    : runtime_(std::move(runtime)),
      context_(std::move(context)),
      ioc_(&ioc),
      services_(std::move(services)),
      js_time_budget_(engine.js_time_budget) {}

ScriptRuntime::~ScriptRuntime() {
  close();
  // pool / Registry / paths: owned by ScriptServices (composition root).
}

void ScriptRuntime::close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  shutting_down_ = true;
  cancel_host_async();
  script_initialized_ = false;
  // Free long-lived JS values while context is still alive.
  script_ns_.reset();
  if (context_.ok()) {
    JS_SetContextOpaque(context_.get(), nullptr);
  }
  // JS_FreeContext (finalizers release native handles) then JS_FreeRuntime.
  context_.reset();
  runtime_.reset();
}

void ScriptRuntime::async_op_begin() noexcept {
  ++outstanding_async_;
}

void ScriptRuntime::async_op_end() noexcept {
  if (outstanding_async_ > 0) {
    --outstanding_async_;
  }
  if (outstanding_async_ == 0) {
    notify_progress();
  }
}

asio::awaitable<void> ScriptRuntime::wait_async_idle(std::chrono::milliseconds timeout) {
  if (outstanding_async_ == 0) {
    co_return;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (outstanding_async_ > 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      vacps::log::warn(
          "wait_async_idle: {} op(s) still outstanding after {}ms",
          outstanding_async_,
          timeout.count());
      co_return;
    }
    auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor);
    timer->expires_after(std::chrono::milliseconds(20));
    progress_waiters_.push_back(timer);
    auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
    progress_waiters_.remove(timer);
  }
  co_return;
}

void ScriptRuntime::cancel_host_async() noexcept {
  shutting_down_ = true;
  ++progress_generation_;
  for (auto& t : progress_waiters_) {
    if (t) {
      t->cancel();
    }
  }
  progress_waiters_.clear();
}

void ScriptRuntime::notify_progress() {
  ++progress_generation_;
  std::list<std::shared_ptr<asio::steady_timer>> waiters;
  waiters.swap(progress_waiters_);
  for (auto& t : waiters) {
    if (t) {
      t->cancel();
    }
  }
}

asio::awaitable<void> ScriptRuntime::wait_progress() {
  // Park until notify_progress / cancel_host_async, with a short fallback so a
  // lost wake (race between settle and waiter registration) cannot stall forever.
  // notify_progress still cancels early for low latency.
  const auto gen = progress_generation_;
  auto executor = co_await asio::this_coro::executor;
  if (progress_generation_ != gen) {
    co_return;
  }
  auto timer = std::make_shared<asio::steady_timer>(executor);
  timer->expires_after(std::chrono::milliseconds(25));
  progress_waiters_.push_back(timer);
  // Re-check after enqueuing: another thread (or earlier post) may have notified.
  if (progress_generation_ != gen) {
    progress_waiters_.remove(timer);
    timer->cancel();
    co_return;
  }
  auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
  (void)ec;
  progress_waiters_.remove(timer);
  co_return;
}

asio::awaitable<bool> ScriptRuntime::wait_progress_or_deadline() {
  // Native I/O wait is NOT under the JS CPU interrupt budget. Only park until
  // notify_progress / cancel_host_async wakes us.
  co_await wait_progress();
  co_return true;
}

Result<Value> ScriptRuntime::eval(
    std::string_view source,
    std::string_view filename,
    int flags) {
  if (closed_ || !ok()) {
    return std::unexpected(Error{"ScriptRuntime closed"});
  }
  InterruptScope guard{runtime_, js_time_budget_};
  auto value = context_.eval(source, filename, flags);
  if (!value) {
    return value;
  }
  if (auto drain = runtime_.drain_jobs(); !drain) {
    return std::unexpected(std::move(drain.error()));
  }
  return value;
}

Result<Value> ScriptRuntime::eval_module(std::string_view source, std::string_view filename) {
  return eval(source, filename, JS_EVAL_TYPE_MODULE);
}

asio::awaitable<VoidResult> ScriptRuntime::await_settled(Value& value) {
  auto* ctx = context_.get();
  if (ctx == nullptr || value.empty()) {
    co_return std::unexpected(Error{"await: empty value"});
  }
  if (!value.is_object()) {
    co_return VoidResult{};
  }

  // Per-turn microtask budget: drain runnable jobs, then yield once if more remain.
  // Jobs already in the QuickJS queue do not wait on native I/O; post is fairness only.
  // CPU interrupt is armed only around each drain_jobs call — not across native waits.
  constexpr std::size_t kMaxJobsPerTurn = 128;

  for (;;) {
    const JSPromiseStateEnum st = JS_PromiseState(ctx, value.get());
    if (st == JS_PROMISE_PENDING) {
      {
        InterruptScope turn{runtime_, js_time_budget_};
        auto drained = runtime_.drain_jobs_budgeted(kMaxJobsPerTurn);
        if (!drained) {
          co_return std::unexpected(std::move(drained.error()));
        }
      }
      if (JS_PromiseState(ctx, value.get()) != JS_PROMISE_PENDING) {
        continue;
      }
      // Never busy-spin: always co_await so other Asio handlers can run.
      if (JS_IsJobPending(runtime_.get())) {
        auto ex = co_await asio::this_coro::executor;
        co_await asio::post(ex, asio::use_awaitable);
      } else {
        // No runnable JS; park until native settle (notify_progress).
        co_await wait_progress();
      }
      continue;
    }
    if (st == JS_PROMISE_REJECTED) {
      Value err_val{ctx, JS_PromiseResult(ctx, value.get())};
      const std::string detail = format_js_exception(ctx, err_val.get());
      co_return std::unexpected(
          Error{std::format("script promise rejected: {}", detail)});
    }
    if (st == JS_PROMISE_FULFILLED) {
      value = Value{ctx, JS_PromiseResult(ctx, value.get())};
      co_return VoidResult{};
    }
    co_return VoidResult{};
  }
}

asio::awaitable<Result<Value>> ScriptRuntime::await_value(Value value) {
  // No wall-clock InterruptScope across the await — only per-turn drain jobs.
  if (auto a = co_await await_settled(value); !a) {
    co_return std::unexpected(std::move(a.error()));
  }
  co_return std::move(value);
}

asio::awaitable<Result<Value>> ScriptRuntime::invoke_export(
    const char* name,
    int argc,
    JSValueConst* argv) {
  if (closed_) {
    co_return std::unexpected(Error{"ScriptRuntime closed"});
  }
  auto* ctx = context_.get();
  if (ctx == nullptr) {
    co_return std::unexpected(Error{"no context"});
  }
  if (script_ns_.empty()) {
    co_return std::unexpected(Error{"business script not loaded"});
  }
  Value fn = Value::get_property_str(ctx, script_ns_.get(), name);
  if (fn.is_exception()) {
    co_return std::unexpected(context_.take_exception_error());
  }
  if (!fn.is_function()) {
    co_return std::unexpected(Error{std::format("export '{}' is not a function", name)});
  }
  // CPU interrupt only for the synchronous JS_Call turn — not native I/O awaits.
  Value out;
  {
    InterruptScope guard{runtime_, js_time_budget_};
    out = Value{ctx, JS_Call(ctx, fn.get(), JS_UNDEFINED, argc, argv)};
  }
  if (out.is_exception()) {
    co_return std::unexpected(context_.take_exception_error());
  }
  if (auto a = co_await await_settled(out); !a) {
    co_return std::unexpected(std::move(a.error()));
  }
  co_return out;
}

asio::awaitable<VoidResult> ScriptRuntime::initialize_from_source(
    std::string_view source,
    std::string_view filename) {
  if (closed_) {
    co_return std::unexpected(Error{"ScriptRuntime closed"});
  }
  auto* ctx = context_.get();
  if (ctx == nullptr) {
    co_return std::unexpected(Error{"no js context"});
  }
  if (source.empty()) {
    co_return std::unexpected(Error{"script file is empty"});
  }

  const std::string filename_owned{filename};
  JSModuleDef* mod = nullptr;
  Value pending;
  {
    // Interrupt only for compile + first eval turn — not while awaiting imports.
    InterruptScope load_guard{runtime_, js_time_budget_};
    Value compiled{
        ctx,
        JS_Eval(
            ctx,
            source.data(),
            source.size(),
            filename_owned.c_str(),
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)};
    if (compiled.is_exception()) {
      co_return std::unexpected(Error{
          std::format("compile script failed: {}", context_.take_exception_error().message)});
    }
    mod = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
    pending = Value{ctx, JS_EvalFunction(ctx, compiled.release())};
    if (pending.is_exception()) {
      co_return std::unexpected(
          Error{std::format("eval script failed: {}", context_.take_exception_error().message)});
    }
  }
  if (auto a = co_await await_settled(pending); !a) {
    co_return std::unexpected(
        Error{std::format("script module failed: {}", a.error().message)});
  }

  script_ns_ = Value{ctx, JS_GetModuleNamespace(ctx, mod)};
  if (script_ns_.is_exception()) {
    script_ns_.reset();
    co_return std::unexpected(
        Error{std::format("module namespace: {}", context_.take_exception_error().message)});
  }

  auto init = co_await invoke_export("initialize", 0, nullptr);
  if (!init) {
    script_ns_.reset();
    co_return std::unexpected(
        Error{std::format("initialize() failed: {}", init.error().message)});
  }
  script_initialized_ = true;
  log::info("business script ready ({})", filename);
  co_return VoidResult{};
}

asio::awaitable<VoidResult> load_and_initialize(
    ScriptRuntime& rt,
    std::string_view script_path) {
  auto src = read_script_file(script_path);
  if (!src) {
    co_return std::unexpected(std::move(src.error()));
  }
  co_return co_await rt.initialize_from_source(*src, script_path);
}

asio::awaitable<VoidResult> ScriptRuntime::shutdown_script() {
  if (closed_ || !script_initialized_) {
    co_return VoidResult{};
  }
  // Mark closing so inbound HTTP handlers stop dispatching into JS during teardown.
  // cancel_host_async() also sets this; idempotent.
  shutting_down_ = true;
  // Leave progress waiters live so await db.close() / other Promises can settle.
  auto sh = co_await invoke_export("shutdown", 0, nullptr);
  script_initialized_ = false;
  script_ns_.reset();
  if (!sh) {
    co_return std::unexpected(
        Error{std::format("shutdown() failed: {}", sh.error().message)});
  }
  co_return VoidResult{};
}

Result<std::shared_ptr<ScriptRuntime>> ScriptRuntime::create(
    asio::io_context& ioc,
    EngineOptions engine,
    std::shared_ptr<ScriptServices> services) {
  if (!services) {
    return std::unexpected(Error{"ScriptRuntime::create: ScriptServices required"});
  }

  auto runtime = Runtime::create(engine.heap_limit_bytes, engine.stack_limit_bytes);
  if (!runtime) {
    return std::unexpected(std::move(runtime.error()));
  }

  auto context = Context::create(*runtime);
  if (!context) {
    return std::unexpected(std::move(context.error()));
  }

  auto host = std::shared_ptr<ScriptRuntime>(new ScriptRuntime(
      std::move(*runtime),
      std::move(*context),
      ioc,
      std::move(engine),
      std::move(services)));

  // Modules resolve ScriptRuntime via context opaque (Promise bridge / services()).
  // vacps:* loaders / globals: install_default_modules() in bindings, after create.
  JS_SetContextOpaque(host->context().get(), host.get());

  log::info(
      "quickjs script runtime ready (js_time_budget_ms={})",
      host->js_time_budget().count());
  return host;
}

}  // namespace vacps::js
