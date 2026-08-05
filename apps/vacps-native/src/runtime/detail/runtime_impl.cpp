#include "runtime/detail/runtime_impl.hpp"

#include "app/log.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <utility>

namespace vacps {

Runtime::Impl::Impl(Runtime& facade, Options options)
    : facade_(facade),
      options_(std::move(options)),
      worker_pool_(std::max<std::size_t>(1, options_.worker_threads)),
      async_(*this),
      callbacks_(*this),
      script_(*this) {}

Runtime::Impl::~Impl() noexcept {
  release_daemon_work();
  if (engine_.is_open()) {
    close_engine();
  }
  phase_.store(Runtime::Phase::closed, std::memory_order_release);
}

JSContext* Runtime::Impl::context() noexcept {
  return engine_.context();
}

runtime::VoidResult Runtime::Impl::initialize() {
  auto opened = engine_.open(options_.engine);
  if (!opened) {
    return opened;
  }

  // Opaque points at the stable public Runtime facade. Host/modules must not
  // overwrite it. Prefer explicit capability injection over context lookup.
  JS_SetContextOpaque(engine_.context(), &facade_);

  phase_.store(Runtime::Phase::initialized, std::memory_order_release);
  return {};
}

runtime::Result<vacps::qjs::OwnedValue> Runtime::Impl::evaluate(
    std::string_view source,
    std::string_view filename,
    int flags) {
  runtime::Result<vacps::qjs::OwnedValue> result;
  {
    runtime::InterruptBudget budget{engine_, options_.engine.js_time_budget};
    result = engine_.evaluate(source, filename, flags);
  }

  if (!result) {
    return result;
  }
  schedule_job_pump();
  return result;
}

void Runtime::Impl::note_fatal(const runtime::Error& error) noexcept {
  report_error(error);
  if (fatal_exit_code_ == 0) {
    fatal_exit_code_ = 1;
  }
}

int Runtime::Impl::run(
    std::move_only_function<runtime::VoidResult() noexcept> startup) {
  // Install the sole artificial keepalive before entering running. Allocation
  // failure leaves phase==initialized so stack unwinding can close the engine
  // normally; it is not a recoverable Runtime state transition.
  daemon_work_.emplace(main_io_.get_executor());
  phase_.store(Runtime::Phase::running, std::memory_order_release);

  if (stop_requested_.load(std::memory_order_acquire)) {
    begin_shutdown();
  }

  if (phase() == Runtime::Phase::running) {
    runtime::VoidResult started = startup();
    if (!started) {
      note_fatal(started.error());
      begin_shutdown();
    } else if (engine_.has_pending_jobs()) {
      schedule_job_pump();
    }
  }

  if (stop_requested_.load(std::memory_order_acquire) &&
      phase() == Runtime::Phase::running) {
    begin_shutdown();
  }

  // Single run: daemon work_guard keeps the loop alive until stop releases
  // it. Natural return means Asio has no outstanding work left.
  main_io_.run();

  close_engine();
  phase_.store(Runtime::Phase::closed, std::memory_order_release);

  join_workers();
  return fatal_exit_code_;
}

void Runtime::Impl::request_stop() noexcept {
  // Sticky intent first — never lose a stop that fails to post.
  stop_requested_.store(true, std::memory_order_release);

  // Non-owning this under the Runtime lifetime contract. A call racing
  // Runtime destruction is a Narrow violation. Do not recover via
  // main_io.stop() — that would abandon outstanding work.
  // noexcept is intentional: allocation failure while posting is fatal.
  runtime::asio::post(main_io_, [this]() noexcept {
    begin_shutdown();
  });
}

runtime::VoidResult Runtime::Impl::post_to_owner(
    std::move_only_function<void() noexcept> f) {
  if (!accepts_owner_post()) {
    return std::unexpected(runtime::Error::invalid_state(
        "owner post rejected: runtime not accepting host work"));
  }
  // Non-owning this under the Runtime lifetime contract.
  runtime::asio::post(main_io_, [this, fn = std::move(f)]() mutable noexcept {
    if (!accepts_owner_post()) {
      // Stopping between admission and execution — drop without JS.
      fn = nullptr;
      return;
    }
    auto local = std::move(fn);
    local();
  });
  return {};
}

void Runtime::Impl::report_error(const runtime::Error& error) noexcept {
  vacps::log::error(
      "runtime: [{}] {}", static_cast<int>(error.code), error.message);
}

void Runtime::Impl::schedule_job_pump() noexcept {
  // Narrow: natural drain keeps the engine live for every posted turn. Only
  // coalesce duplicate schedules. On the single owner thread, a false pending
  // check cannot race a foreign QuickJS queue mutation; later native
  // settlement schedules again when it actually enqueues a reaction job.
  if (job_pump_scheduled_ || !engine_.has_pending_jobs()) {
    return;
  }
  job_pump_scheduled_ = true;
  // Non-owning this under the Runtime lifetime contract. noexcept makes an
  // allocation failure in the internal scheduler fail fast.
  runtime::asio::post(main_io_, [this]() noexcept {
    job_pump_scheduled_ = false;
    auto result = run_job_turn();
    if (!result) {
      report_error(result.error());
    }
    if (engine_.has_pending_jobs()) {
      schedule_job_pump();
    }
  });
}

runtime::VoidResult Runtime::Impl::run_job_turn() {
  runtime::InterruptBudget budget{engine_, options_.engine.js_time_budget};

  for (std::size_t i = 0; i < kJobsPerTurn; ++i) {
    auto state = engine_.execute_one_pending_job();
    if (!state) {
      return std::unexpected(std::move(state.error()));
    }
    if (*state == runtime::JobState::empty) {
      return {};
    }
  }
  return {};
}

void Runtime::Impl::begin_shutdown() noexcept {
  Runtime::Phase expected = Runtime::Phase::running;
  if (!phase_.compare_exchange_strong(
          expected, Runtime::Phase::stopping, std::memory_order_acq_rel)) {
    // Not running (already stopping/closed/initialized) — expected transition.
    return;
  }
  stop_requested_.store(true, std::memory_order_release);

  // Cooperative cancellation for every managed coroutine / reverse await.
  // Engine stays open; only the artificial keepalive is released so Asio can
  // drain real outstanding work naturally. request_stop is noexcept.
  shutdown_stop_source_.request_stop();
  release_daemon_work();
}

void Runtime::Impl::close_engine() noexcept {
  job_pump_scheduled_ = false;
  engine_.close();
}

void Runtime::Impl::release_daemon_work() noexcept {
  // optional reset / work_guard destruction are noexcept.
  daemon_work_.reset();
}

void Runtime::Impl::join_workers() noexcept {
  worker_pool_.join();
}

}  // namespace vacps
