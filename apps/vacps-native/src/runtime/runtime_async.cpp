#include "runtime/runtime_async.hpp"

#include "runtime/detail/runtime_impl.hpp"

#include <boost/asio/strand.hpp>

#include <utility>

namespace vacps {

Runtime::Async::Async(Impl& impl) noexcept : impl_(impl) {}

runtime::asio::any_io_executor Runtime::Async::executor() const {
  return impl_.main_executor();
}

JSContext* Runtime::Async::owner_context() const noexcept {
  return impl_.owner_context();
}

std::stop_token Runtime::Async::shutdown_stop_token() const noexcept {
  return impl_.shutdown_stop_token();
}

runtime::asio::any_io_executor Runtime::Async::worker_executor() const {
  return impl_.worker_executor();
}

Runtime::Async::SerialWorker Runtime::Async::make_serial_worker() const {
  return SerialWorker{runtime::asio::make_strand(worker_executor())};
}

void Runtime::Async::schedule_job_pump() noexcept {
  impl_.schedule_job_pump();
}

void Runtime::Async::report_error(const runtime::Error& error) noexcept {
  impl_.report_error(error);
}

void Runtime::Async::settle_or_report(
    runtime::PromiseCapability& capability,
    runtime::Error err) noexcept {
  // Natural drain: engine and capability are live for this settle. Perform
  // the single reject attempt and report any operational settlement error.
  auto rejected = capability.reject_error(std::move(err));
  if (!rejected) {
    impl_.report_error(rejected.error());
  }
}

void Runtime::Async::complete_promise(
    runtime::PromiseCapability& capability,
    std::exception_ptr ep,
    runtime::Result<vacps::qjs::OwnedValue> result) noexcept {
  // Sole post-coroutine settlement point. co_spawn delivers either an
  // exception_ptr or the returned Result on the owner executor.
  if (ep != nullptr) {
    settle_or_report(
        capability,
        runtime::error_from_exception_ptr(
            std::move(ep),
            runtime::Errc::native_failure,
            "unknown exception in async task"));
  } else if (!result) {
    settle_or_report(capability, std::move(result.error()));
  } else {
    auto resolved = capability.resolve(result->get());
    if (!resolved) {
      report_error(resolved.error());
    }
  }

  schedule_job_pump();
}

}  // namespace vacps
