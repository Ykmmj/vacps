#include "runtime/runtime_async.hpp"

#include "runtime/detail/runtime_impl.hpp"

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

}  // namespace vacps
