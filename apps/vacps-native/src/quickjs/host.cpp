#include "quickjs/host.hpp"

#include "quickjs/cstring.hpp"
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

Host::Host(Runtime runtime, Context context, asio::io_context& ioc, Config cfg)
    : runtime_(std::move(runtime)),
      context_(std::move(context)),
      ioc_(&ioc),
      pool_(std::make_unique<asio::thread_pool>(2)),
      processes_(std::make_unique<vacps::process::Registry>(ioc.get_executor())),
      use_stream_file_(vacps::fs::probe_io_uring()),
      cfg_(std::move(cfg)) {}

Host::~Host() {
  cancel_host_async();
  if (processes_) {
    processes_->shutdown();
  }
  if (pool_) {
    pool_->stop();
    pool_->join();
  }
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
  if (shutting_down_) {
    co_return;
  }
  const auto gen = progress_generation_;
  auto executor = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(executor);
  timer->expires_at(asio::steady_timer::time_point::max());
  progress_waiters_.push_back(timer);
  auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
  (void)ec;
  progress_waiters_.remove(timer);
  // gen may have advanced (real progress) or we were cancelled for shutdown.
  (void)gen;
  co_return;
}

Result<Value> Host::eval(
    std::string_view source,
    std::string_view filename,
    int flags) {
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
  constexpr std::size_t kMaxJobsPerTurn = 128;

  for (;;) {
    const JSPromiseStateEnum st = JS_PromiseState(ctx, value.get());
    if (st == JS_PROMISE_PENDING) {
      auto drained = runtime_.drain_jobs_budgeted(kMaxJobsPerTurn);
      if (!drained) {
        co_return std::unexpected(std::move(drained.error()));
      }
      if (JS_PromiseState(ctx, value.get()) != JS_PROMISE_PENDING) {
        continue;
      }
      // Never busy-spin: always co_await so other Asio handlers can run.
      if (JS_IsJobPending(runtime_.get())) {
        // Runnable microtasks remain after the per-turn budget;
        // yield to the io_context for fairness.
        auto ex = co_await asio::this_coro::executor;
        co_await asio::post(ex, asio::use_awaitable);
      } else {
        // No runnable JS; wait for native settle + notify_progress.
        co_await wait_progress();
      }
      continue;
    }
    if (st == JS_PROMISE_REJECTED) {
      Value err_val{ctx, JS_PromiseResult(ctx, value.get())};
      auto cs = CString::from_value(ctx, err_val.get());
      if (cs.empty()) {
        co_return std::unexpected(Error{"script promise rejected"});
      }
      co_return std::unexpected(
          Error{std::format("script promise rejected: {}", cs.view())});
    }
    if (st == JS_PROMISE_FULFILLED) {
      value = Value{ctx, JS_PromiseResult(ctx, value.get())};
      co_return VoidResult{};
    }
    co_return VoidResult{};
  }
}

asio::awaitable<Result<Value>> Host::await_value(Value value) {
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
  Value out{ctx, JS_Call(ctx, fn.get(), JS_UNDEFINED, argc, argv)};
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
  // COMPILE_ONLY: value is a module function; EvalFunction consumes it.
  Value compiled{
      ctx,
      JS_Eval(
          ctx,
          src->data(),
          src->size(),
          filename.c_str(),
          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)};
  if (compiled.is_exception()) {
    co_return std::unexpected(
        Error{std::format("compile script failed: {}", context_.take_exception_error().message)});
  }
  auto* mod = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
  Value pending{ctx, JS_EvalFunction(ctx, compiled.release())};
  if (pending.is_exception()) {
    co_return std::unexpected(
        Error{std::format("eval script failed: {}", context_.take_exception_error().message)});
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
    cancel_host_async();
    co_return VoidResult{};
  }
  auto sh = co_await invoke_export("shutdown", 0, nullptr);
  script_initialized_ = false;
  script_ns_.reset();
  cancel_host_async();
  if (!sh) {
    co_return std::unexpected(
        Error{std::format("shutdown() failed: {}", sh.error().message)});
  }
  co_return VoidResult{};
}

Result<std::shared_ptr<Host>> Host::create(
    const Config& cfg,
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

  auto host = std::shared_ptr<Host>(
      new Host(std::move(*runtime), std::move(*context), ioc, cfg));

  // Modules resolve Host via JS_GetContextOpaque.
  JS_SetContextOpaque(host->context().get(), host.get());

  if (auto mods = install_native_modules(host->runtime().get(), host->context().get()); !mods) {
    return std::unexpected(std::move(mods.error()));
  }

  log::info(
      "quickjs host ready (fs content={})",
      host->use_stream_file() ? "stream_file/io_uring" : "thread_pool");
  return host;
}

}  // namespace vacps::js
