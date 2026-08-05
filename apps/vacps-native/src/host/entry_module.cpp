#include "host/entry_module.hpp"

#include <utility>

namespace vacps::host {

asio::awaitable<runtime::VoidResult> EntryModule::load_and_initialize(
    Runtime& runtime,
    std::string_view source,
    std::string_view filename,
    std::chrono::milliseconds timeout) {
  // evaluate_module is synchronous — consume caller string_views before any
  // suspension point so they need not outlive this coroutine.
  auto evaluated = runtime.script().evaluate_module(source, filename);
  if (!evaluated) {
    co_return std::unexpected(std::move(evaluated.error()));
  }

  JSModuleDef* definition = evaluated->definition;
  vacps::qjs::OwnedValue completion = std::move(evaluated->completion);

  {
    auto settled = co_await runtime.await_value(
        std::move(completion),
        runtime::JsAwaitOptions{.timeout = timeout});
    if (!settled) {
      // Failed module evaluation completion: leave no stored definition.
      co_return std::unexpected(std::move(settled.error()));
    }
    // Drop fulfilled value; do not retain long-lived JSValues.
  }

  // Evaluation succeeded — retain the non-owning definition for initialize.
  definition_ = definition;

  auto init_call = runtime.script().invoke_export(definition_, "initialize");
  if (!init_call) {
    co_return std::unexpected(std::move(init_call.error()));
  }

  {
    auto init_settled = co_await runtime.await_value(
        std::move(*init_call),
        runtime::JsAwaitOptions{.timeout = timeout});
    if (!init_settled) {
      co_return std::unexpected(std::move(init_settled.error()));
    }
  }

  co_return runtime::success();
}

asio::awaitable<runtime::VoidResult> EntryModule::shutdown(
    Runtime& runtime,
    std::chrono::milliseconds timeout) {
  auto call = runtime.script().invoke_export(definition_, "shutdown");
  if (!call) {
    co_return std::unexpected(std::move(call.error()));
  }

  auto settled = co_await runtime.await_value(
      std::move(*call),
      runtime::JsAwaitOptions{.timeout = timeout});
  if (!settled) {
    co_return std::unexpected(std::move(settled.error()));
  }

  co_return runtime::success();
}

void EntryModule::reset() noexcept {
  definition_ = nullptr;
}

}  // namespace vacps::host
