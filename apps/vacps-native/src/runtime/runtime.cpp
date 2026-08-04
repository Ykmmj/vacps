#include "runtime/runtime.hpp"

#include "runtime/detail/runtime_impl.hpp"

#include <utility>

namespace vacps {

Runtime::Runtime(Options options)
    : impl_(std::make_unique<Impl>(*this, std::move(options))) {}

Runtime::~Runtime() noexcept = default;

runtime::VoidResult Runtime::initialize() {
  return impl_->initialize();
}

int Runtime::run(
    std::move_only_function<runtime::VoidResult() noexcept> startup) {
  return impl_->run(std::move(startup));
}

void Runtime::request_stop() noexcept {
  impl_->request_stop();
}

runtime::VoidResult Runtime::post_to_owner(
    std::move_only_function<void() noexcept> f) {
  return impl_->post_to_owner(std::move(f));
}

Runtime::Async& Runtime::async() noexcept {
  return impl_->async();
}

const Runtime::Async& Runtime::async() const noexcept {
  return impl_->async();
}

Runtime::Callbacks& Runtime::callbacks() noexcept {
  return impl_->callbacks();
}

const Runtime::Callbacks& Runtime::callbacks() const noexcept {
  return impl_->callbacks();
}

Runtime::Script& Runtime::script() noexcept {
  return impl_->script();
}

const Runtime::Script& Runtime::script() const noexcept {
  return impl_->script();
}

JSContext* Runtime::context() noexcept {
  return impl_->context();
}

runtime::Result<vacps::qjs::OwnedValue> Runtime::evaluate(
    std::string_view source,
    std::string_view filename,
    int flags) {
  return impl_->evaluate(source, filename, flags);
}

runtime::asio::awaitable<runtime::Result<vacps::qjs::OwnedValue>>
Runtime::await_value(
    vacps::qjs::OwnedValue value,
    runtime::JsAwaitOptions options) {
  return runtime::detail::JsPromiseAwaitAccess::await_value(
      *this, std::move(value), options);
}

runtime::asio::any_io_executor Runtime::main_executor() noexcept {
  return impl_->main_executor();
}

}  // namespace vacps
