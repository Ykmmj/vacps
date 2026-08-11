#include "modules/bindings.hpp"

#include "binding/class.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "modules/terminal_convert.hpp"
#include "process/runtime.hpp"
#include "runtime/error.hpp"
#include "runtime/runtime_async.hpp"
#include "terminal/terminal.hpp"

#include <quickjs.h>

#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;
namespace term = vacps::terminal;
namespace tm = vacps::js::terminal_module;

constexpr const char* k_terminal_exports[] = {"Terminal"};

[[nodiscard]] runtime::VoidResult map_void(vacps::VoidResult result) {
  if (!result) {
    return std::unexpected(runtime::Error::from_domain(std::move(result.error())));
  }
  return {};
}

template <class T>
[[nodiscard]] runtime::Result<T> map_result(vacps::Result<T> result) {
  if (!result) {
    return std::unexpected(runtime::Error::from_domain(std::move(result.error())));
  }
  return std::move(*result);
}

[[nodiscard]] runtime::Error cancelled(std::string_view operation) {
  return runtime::Error::cancelled_op(std::string{operation});
}

template <class Callback>
struct StopBridge {
  std::stop_callback<Callback> callback;
  StopBridge(std::stop_token token, Callback fn)
      : callback(std::move(token), std::move(fn)) {}
};

[[nodiscard]] auto make_stop_bridge(
    std::stop_token stop,
    const std::shared_ptr<term::Terminal>& terminal) {
  std::weak_ptr<term::Terminal> weak = terminal;
  return StopBridge{
      std::move(stop),
      [weak = std::move(weak)]() noexcept {
        if (auto terminal = weak.lock()) terminal->dispose();
      }};
}

[[nodiscard]] binding::Result<std::shared_ptr<term::Terminal>> construct_terminal(
    const binding::CallbackInfo& info) {
  if (auto argc = info.check_argc(1, "Terminal"); !argc) {
    return std::unexpected(std::move(argc.error()));
  }
  auto command = info.arg<std::string>(0);
  if (!command) return std::unexpected(std::move(command.error()));
  if (command->empty()) {
    return std::unexpected(binding::Error::type("command must be non-empty"));
  }
  auto args = info.arg<tm::OptionalStringArgs>(1);
  if (!args) return std::unexpected(std::move(args.error()));
  auto options = info.arg<std::optional<tm::OptionsDecode>>(2);
  if (!options) return std::unexpected(std::move(options.error()));

  std::vector<std::string> argv;
  argv.reserve(1 + args->args.size());
  argv.push_back(std::move(*command));
  for (auto& argument : args->args) argv.push_back(std::move(argument));

  term::StartOptions start_options;
  if (options->has_value()) start_options = std::move((*options)->options);
  process::ProcessRuntime& runtime = process_runtime_from_context(info.context());
  return std::make_shared<term::Terminal>(
      runtime.executor(), runtime.budget(), std::move(argv), std::move(start_options));
}

int initialize_terminal(JSContext* ctx, JSModuleDef* module) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder builder{env};
    using TerminalBuilder = binding::ClassBuilder<term::Terminal>;

    auto terminal =
        TerminalBuilder{env, "Terminal"}
            .constructor(
                [](const binding::CallbackInfo& info)
                    -> binding::Result<std::shared_ptr<term::Terminal>> {
                  return construct_terminal(info);
                },
                3)
            .async_method(
                "start",
                [](std::stop_token stop, std::shared_ptr<term::Terminal> self)
                    -> runtime::Task<void> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("start"));
                  }
                  auto bridge = make_stop_bridge(stop, self);
                  auto result = map_void(co_await self->start());
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("start"));
                  }
                  co_return result;
                },
                0)
            .async_method(
                "write",
                [](std::stop_token stop,
                   std::shared_ptr<term::Terminal> self,
                   tm::WritePayload payload) -> runtime::Task<std::size_t> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("write"));
                  }
                  auto bridge = make_stop_bridge(stop, self);
                  auto result = map_result(
                      co_await self->write(std::move(payload.data)));
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("write"));
                  }
                  co_return result;
                },
                1)
            .async_method(
                "read",
                [](std::stop_token stop,
                   std::shared_ptr<term::Terminal> self,
                   tm::ReadOptionsDecode options)
                    -> runtime::Task<term::ReadResult> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("read"));
                  }
                  auto bridge = make_stop_bridge(stop, self);
                  auto result = co_await self->read(options.options);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("read"));
                  }
                  co_return result;
                },
                1)
            .async_method(
                "resize",
                [](std::stop_token,
                   std::shared_ptr<term::Terminal> self,
                   tm::Columns columns,
                   tm::Rows rows) -> runtime::Task<void> {
                  co_return map_void(self->resize(columns.value, rows.value));
                },
                2)
            .async_method(
                "signal",
                [](std::stop_token,
                   std::shared_ptr<term::Terminal> self,
                   tm::Signal signal) -> runtime::Task<void> {
                  co_return map_void(self->signal(signal.value));
                },
                1)
            .method(
                "snapshot",
                [](const term::Terminal& self) { return self.snapshot(); },
                0)
            .async_method(
                "waitForExit",
                [](std::stop_token stop,
                   std::shared_ptr<term::Terminal> self,
                   tm::WaitTimeout timeout)
                    -> runtime::Task<term::ExitWaitResult> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("waitForExit"));
                  }
                  auto bridge = make_stop_bridge(stop, self);
                  auto result = co_await self->wait_for_exit(timeout.value);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled("waitForExit"));
                  }
                  co_return result;
                },
                1)
            .async_method(
                "close",
                [](std::stop_token,
                   std::shared_ptr<term::Terminal> self,
                   tm::CloseGrace grace) -> runtime::Task<void> {
                  co_return map_void(co_await self->async_close(grace.value));
                },
                1)
            .commit();

    if (!terminal) {
      (void)binding::throw_error(ctx, terminal.error());
      return -1;
    }
    if (builder.set_export(module, "Terminal", std::move(*terminal)) != 0) {
      return -1;
    }
    return 0;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return -1;
  } catch (const std::exception& error) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, error.what());
    }
    return -1;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "terminal module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_terminal(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* module = JS_NewCModule(ctx, name, initialize_terminal);
    if (module == nullptr) return nullptr;
    for (const char* export_name : k_terminal_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, module, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "terminal module: declare_export failed");
        }
        return nullptr;
      }
    }
    return module;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return nullptr;
  } catch (const std::exception& error) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, error.what());
    }
    return nullptr;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "terminal module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
