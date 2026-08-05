#include "modules/bindings.hpp"

#include "binding/async_function.hpp"
#include "binding/error.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "runtime/error.hpp"
#include "runtime/runtime_async.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <quickjs.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <stop_token>
#include <string>
#include <utility>

namespace vacps::js {
namespace {

namespace asio = boost::asio;
namespace binding = vacps::binding;

constexpr const char* k_timer_exports[] = {
    "sleep",
};

/**
 * Wait on the Runtime owner executor without occupying a worker thread.
 *
 * Contract: Narrow C++ mechanism.
 * Preconditions: called by Runtime::Async on its live main executor.
 * The public JavaScript boundary is Wide: binding decode accepts only a
 * uint32 millisecond delay and throws synchronously before Promise creation
 * for every other value.
 */
runtime::Task<void> sleep_task(
    std::stop_token stop,
    std::uint32_t delay_ms) {
  if (stop.stop_requested()) {
    co_return std::unexpected(runtime::Error::cancelled_op("timer.sleep"));
  }

  auto executor = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(
      executor, std::chrono::milliseconds{delay_ms});
  std::weak_ptr<asio::steady_timer> weak_timer = timer;

  // Always post the cancellation edge. If stop is requested while the
  // callback is being installed, the posted cancel cannot run on the single
  // owner executor until async_wait has been initiated. The weak capture also
  // makes a late posted callback harmless after normal expiry.
  std::stop_callback on_stop{
      stop,
      [executor, weak_timer = std::move(weak_timer)]() noexcept {
        asio::post(
            executor,
            [weak_timer]() noexcept {
              if (auto live_timer = weak_timer.lock()) {
                (void)live_timer->cancel();
              }
            });
      }};

  auto [ec] = co_await timer->async_wait(asio::as_tuple);
  if (!ec) {
    co_return runtime::success();
  }
  if (ec == asio::error::operation_aborted) {
    co_return std::unexpected(runtime::Error::cancelled_op("timer.sleep"));
  }
  co_return std::unexpected(runtime::Error::native_io(
      std::string{"timer.sleep: "} + ec.message(),
      "timer.sleep",
      ec.value()));
}

int initialize_timer(JSContext* ctx, JSModuleDef* module) noexcept {
  try {
    binding::Env env{ctx, &async_runtime_from_context(ctx)};
    binding::ModuleBuilder builder{env};
    qjs::OwnedValue sleep = binding::create_async_function(
        env, "sleep", &sleep_task, 1);
    return builder.set_export(module, "sleep", std::move(sleep));
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "timer module init failed");
    }
  }
  return -1;
}

}  // namespace

JSModuleDef* init_module_timer(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* module = JS_NewCModule(ctx, name, initialize_timer);
    if (module == nullptr) {
      return nullptr;
    }
    for (const char* export_name : k_timer_exports) {
      if (binding::ModuleBuilder::declare_export(
              ctx, module, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "timer module: declare_export failed");
        }
        return nullptr;
      }
    }
    return module;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "timer module load failed");
    }
  }
  return nullptr;
}

}  // namespace vacps::js
