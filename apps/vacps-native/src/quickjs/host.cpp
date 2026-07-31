#include "quickjs/host.hpp"

#include "quickjs/js_bridge.hpp"
#include "quickjs/modules.hpp"
#include "app/log.hpp"
#include "app/version.hpp"
#include "fs/io_uring_probe.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fstream>
#include <format>
#include <utility>

namespace vacps::js {
namespace {

Result<std::string> read_file(std::string_view path) {
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    return std::unexpected(Error{std::format("cannot read script: {}", path)});
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

Host::Host(
    Runtime runtime,
    Context context,
    asio::io_context& ioc,
    HostOptions opts,
    vacps::fs::PathSandbox path_sandbox)
    : runtime_(std::move(runtime)),
      context_(std::move(context)),
      ioc_(&ioc),
      pool_(std::make_unique<asio::thread_pool>(2)),
      db_pool_(std::make_unique<asio::thread_pool>(1)),
      processes_(std::make_unique<vacps::process::Registry>(ioc.get_executor())),
      use_stream_file_(vacps::fs::probe_io_uring()),
      data_dir_(std::move(opts.data_dir)),
      ca_bundle_(std::move(opts.ca_bundle)),
      path_sandbox_(std::move(path_sandbox)),
      js_time_budget_(opts.js_time_budget) {}

Host::~Host() {
  cancel_host_async();
  if (processes_) {
    processes_->shutdown();
  }
  if (db_pool_) {
    db_pool_->stop();
    db_pool_->join();
  }
  if (pool_) {
    pool_->stop();
    pool_->join();
  }
}

void Host::async_op_begin() noexcept {
  ++outstanding_async_;
}

void Host::async_op_end() noexcept {
  if (outstanding_async_ > 0) {
    --outstanding_async_;
  }
  if (outstanding_async_ == 0) {
    notify_progress();
  }
}

asio::awaitable<void> Host::wait_async_idle(std::chrono::milliseconds timeout) {
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

void Host::cancel_host_async() noexcept {
  shutting_down_ = true;
  ++progress_generation_;
  for (auto& t : progress_waiters_) {
    if (t) {
      t->cancel();
    }
  }
  progress_waiters_.clear();
}

void Host::notify_progress() {
  ++progress_generation_;
  std::list<std::shared_ptr<asio::steady_timer>> waiters;
  waiters.swap(progress_waiters_);
  for (auto& t : waiters) {
    if (t) {
      t->cancel();
    }
  }
}

asio::awaitable<void> Host::wait_progress() {
  // Always park on a timer — never busy-return. cancel_host_async() cancels waiters.
  const auto gen = progress_generation_;
  auto executor = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(executor);
  timer->expires_at(asio::steady_timer::time_point::max());
  progress_waiters_.push_back(timer);
  auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
  (void)ec;
  progress_waiters_.remove(timer);
  (void)gen;
  co_return;
}

asio::awaitable<bool> Host::wait_progress_or_deadline() {
  // Native I/O wait is NOT under the JS CPU interrupt budget. Only park until
  // notify_progress / cancel_host_async wakes us.
  co_await wait_progress();
  co_return true;
}

Result<Value> Host::eval(
    std::string_view source,
    std::string_view filename,
    int flags) {
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

Result<Value> Host::eval_module(std::string_view source, std::string_view filename) {
  return eval(source, filename, JS_EVAL_TYPE_MODULE);
}

asio::awaitable<VoidResult> Host::await_settled(Value& value) {
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

asio::awaitable<Result<Value>> Host::await_value(Value value) {
  // No wall-clock InterruptScope across the await — only per-turn drain jobs.
  if (auto a = co_await await_settled(value); !a) {
    co_return std::unexpected(std::move(a.error()));
  }
  co_return std::move(value);
}

asio::awaitable<Result<Value>> Host::invoke_export(
    const char* name,
    int argc,
    JSValueConst* argv) {
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

asio::awaitable<VoidResult> Host::load_and_initialize(std::string_view script_path) {
  auto* ctx = context_.get();
  if (ctx == nullptr) {
    co_return std::unexpected(Error{"no js context"});
  }

  auto src = read_file(script_path);
  if (!src) {
    co_return std::unexpected(std::move(src.error()));
  }
  if (src->empty()) {
    co_return std::unexpected(Error{"script file is empty"});
  }

  const std::string filename{script_path};
  JSModuleDef* mod = nullptr;
  Value pending;
  {
    // Interrupt only for compile + first eval turn — not while awaiting imports.
    InterruptScope load_guard{runtime_, js_time_budget_};
    Value compiled{
        ctx,
        JS_Eval(
            ctx,
            src->data(),
            src->size(),
            filename.c_str(),
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
  log::info("business script ready ({})", script_path);
  co_return VoidResult{};
}

asio::awaitable<VoidResult> Host::shutdown_script() {
  if (!script_initialized_) {
    co_return VoidResult{};
  }
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

Result<std::shared_ptr<Host>> Host::create(
    asio::io_context& ioc,
    HostOptions opts) {
  auto runtime = Runtime::create(opts.heap_limit_bytes, opts.stack_limit_bytes);
  if (!runtime) {
    return std::unexpected(std::move(runtime.error()));
  }

  auto context = Context::create(*runtime);
  if (!context) {
    return std::unexpected(std::move(context.error()));
  }

  auto sandbox = vacps::fs::PathSandbox::create(opts.data_dir, opts.fs_extra_roots);

  auto host = std::shared_ptr<Host>(new Host(
      std::move(*runtime),
      std::move(*context),
      ioc,
      std::move(opts),
      std::move(sandbox)));

  // Modules resolve Host via JS_GetContextOpaque.
  JS_SetContextOpaque(host->context().get(), host.get());

  if (auto mods = install_native_modules(host->runtime().get(), host->context().get()); !mods) {
    return std::unexpected(std::move(mods.error()));
  }

  log::info(
      "quickjs host ready (fs content={} js_time_budget_ms={} fs_roots={})",
      host->use_stream_file() ? "stream_file/io_uring" : "thread_pool",
      host->js_time_budget().count(),
      host->path_sandbox().roots().size());
  return host;
}

}  // namespace vacps::js
