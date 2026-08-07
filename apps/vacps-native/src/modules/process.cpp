#include "modules/bindings.hpp"

#include "binding/async_function.hpp"
#include "binding/class.hpp"
#include "binding/function.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "modules/process_convert.hpp"
#include "process/process.hpp"
#include "process/runtime.hpp"
#include "runtime/error.hpp"
#include "runtime/js_encode.hpp"
#include "runtime/runtime_async.hpp"

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
namespace proc = vacps::process;
namespace pm = vacps::js::process_module;

constexpr const char* k_process_exports[] = {
    "Process",
    "run",
};

template <class T>
[[nodiscard]] runtime::Result<T> map_proc_result(vacps::Result<T> r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return std::move(*r);
}

[[nodiscard]] runtime::VoidResult map_proc_void(vacps::VoidResult r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return {};
}

[[nodiscard]] runtime::Error cancelled_err(std::string_view op) {
  return runtime::Error::cancelled_op(std::string{op});
}

/**
 * stop_token → dispose request. dispose() itself only posts onto the owner
 * executor; the callback never mutates Process::State on the requesting thread.
 * Must outlive the co_await it guards.
 */
template <class Cb>
struct StopBridge {
  std::stop_callback<Cb> cb;
  StopBridge(std::stop_token token, Cb cb_fn)
      : cb(std::move(token), std::move(cb_fn)) {}
};

[[nodiscard]] auto make_process_stop_bridge(
    std::stop_token stop,
    const std::shared_ptr<proc::Process>& self) {
  std::weak_ptr<proc::Process> weak = self;
  return StopBridge{
      std::move(stop),
      [weak = std::move(weak)]() noexcept {
        if (auto process = weak.lock()) {
          process->dispose();
        }
      }};
}

[[nodiscard]] binding::Result<std::shared_ptr<proc::Process>> construct_process(
    const binding::CallbackInfo& info) {
  JSContext* ctx = info.context();

  if (auto argc = info.check_argc(1, "Process"); !argc) {
    return std::unexpected(std::move(argc.error()));
  }

  // Composition is caller-established at catalog install (Narrow).
  proc::ProcessRuntime& pr = process_runtime_from_context(ctx);

  auto command = info.arg<std::string>(0);
  if (!command) {
    return std::unexpected(std::move(command.error()));
  }
  if (command->empty()) {
    return std::unexpected(binding::Error::type("command must be non-empty"));
  }

  auto args_dec = info.arg<pm::OptionalStringArgs>(1);
  if (!args_dec) {
    return std::unexpected(std::move(args_dec.error()));
  }

  auto opt_dec = info.arg<std::optional<pm::ProcessOptionsDecode>>(2);
  if (!opt_dec) {
    return std::unexpected(std::move(opt_dec.error()));
  }

  proc::StartOptions opts{};
  if (opt_dec->has_value()) {
    opts = (*opt_dec)->opts;
    if (!(*opt_dec)->stdin_specified) {
      // Process class default: stdin pipe/open.
      opts.close_stdin = false;
    }
  } else {
    opts.close_stdin = false;
  }

  std::vector<std::string> argv;
  argv.reserve(1 + args_dec->args.size());
  argv.push_back(std::move(*command));
  for (auto& a : args_dec->args) {
    argv.push_back(std::move(a));
  }

  auto created = pr.create(std::move(argv), std::move(opts));
  if (!created) {
    return std::unexpected(
        binding::Error::internal(std::move(created.error().message)));
  }
  return std::move(*created);
}

/**
 * Module init (phase 2): ClassBuilder + free `run` export.
 * No C++ exception may escape this C callback.
 *
 * Runtime::Async and ProcessRuntime are recovered from the mandatory JSRuntime
 * composition opaque.
 */
int initialize_process(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder mod{env};

    // Snapshot non-owning ProcessRuntime* at init (stable for Runtime lifetime).
    proc::ProcessRuntime* pr_capture = &process_runtime_from_context(ctx);

    using ProcessBuilder = binding::ClassBuilder<proc::Process>;

    auto committed =
        ProcessBuilder{env, "Process"}
            .constructor(
                [](const binding::CallbackInfo& info)
                    -> binding::Result<std::shared_ptr<proc::Process>> {
                  return construct_process(info);
                },
                3)
            .async_method(
                "start",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self) mutable
                    -> runtime::Task<void> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("start"));
                  }
                  auto bridge = make_process_stop_bridge(stop, self);
                  auto started = map_proc_void(co_await self->start());
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("start"));
                  }
                  co_return started;
                },
                0)
            .async_method(
                "write",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self,
                   pm::WritePayload payload,
                   pm::CloseStdin close_stdin) mutable
                    -> runtime::Task<std::size_t> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("write"));
                  }
                  auto bridge = make_process_stop_bridge(stop, self);
                  auto written = map_proc_result(
                      co_await self->write(
                          std::move(payload.data), close_stdin.value));
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("write"));
                  }
                  co_return written;
                },
                2)
            .async_method(
                "read",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self,
                   pm::ReadOptionsDecode decoded) mutable
                    -> runtime::Task<proc::ReadResult> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("read"));
                  }
                  auto bridge = make_process_stop_bridge(stop, self);
                  proc::ReadResult result =
                      co_await self->read(std::move(decoded.options));
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("read"));
                  }
                  co_return result;
                },
                1)
            .async_method(
                "waitForExit",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self,
                   pm::ExitWait wait) mutable
                    -> runtime::Task<proc::ExitWaitResult> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("waitForExit"));
                  }
                  auto bridge = make_process_stop_bridge(stop, self);
                  proc::ExitWaitResult result =
                      co_await self->wait_for_exit(wait.timeout);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("waitForExit"));
                  }
                  co_return result;
                },
                1)
            .method(
                "snapshot",
                [](const proc::Process& self,
                   pm::SnapshotOptionsDecode decoded)
                    -> proc::ProcessSnapshot {
                  return self.snapshot(
                      decoded.stdout_bytes, decoded.stderr_bytes);
                },
                1)
            .async_method(
                "wait",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self) mutable
                    -> runtime::Task<proc::RunResult> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("wait"));
                  }
                  auto bridge = make_process_stop_bridge(stop, self);
                  auto waited = map_proc_result(co_await self->wait());
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("wait"));
                  }
                  co_return waited;
                },
                0)
            .async_method(
                "terminate",
                [](std::stop_token stop,
                   std::shared_ptr<proc::Process> self,
                   pm::TerminateSignal sig,
                   pm::GracePeriod grace) mutable
                    -> runtime::Task<void> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("terminate"));
                  }
                  // Resolves after the signal request, not after exit.
                  co_return map_proc_void(
                      self->terminate(sig.signo, grace.value));
                },
                2)
            .async_method(
                "close",
                [](std::stop_token /*stop*/,
                   std::shared_ptr<proc::Process> self) mutable
                    -> runtime::Task<void> {
                  // Explicit close ignores injected stop (same as Server.close).
                  co_return map_proc_void(co_await self->async_close());
                },
                0)
            .commit();

    if (!committed) {
      (void)binding::throw_error(ctx, committed.error());
      return -1;
    }
    if (mod.set_export(m, "Process", std::move(*committed)) != 0) {
      return -1;
    }

    // run(command, args?, options?) → Promise<ProcessResult>
    // Empty command and options decode fail synchronously before Promise.
    {
      qjs::OwnedValue run_fn = binding::create_function(
          env,
          "run",
          [pr_capture, async](
              const binding::CallbackInfo& info) mutable -> qjs::OwnedValue {
            JSContext* jc = info.context();

            if (auto argc = info.check_argc(1, "run"); !argc) {
              return qjs::OwnedValue::take(
                  jc, binding::throw_error(jc, argc.error()));
            }

            auto command = info.arg<std::string>(0);
            if (!command) {
              return qjs::OwnedValue::take(
                  jc, binding::throw_error(jc, command.error()));
            }
            if (command->empty()) {
              return qjs::OwnedValue::take(
                  jc,
                  binding::throw_type(jc, "command must be non-empty"));
            }

            auto args_dec = info.arg<pm::OptionalStringArgs>(1);
            if (!args_dec) {
              return qjs::OwnedValue::take(
                  jc, binding::throw_error(jc, args_dec.error()));
            }

            auto opt_dec =
                info.arg<std::optional<pm::ProcessOptionsDecode>>(2);
            if (!opt_dec) {
              return qjs::OwnedValue::take(
                  jc, binding::throw_error(jc, opt_dec.error()));
            }

            proc::StartOptions opts{};
            if (opt_dec->has_value()) {
              opts = (*opt_dec)->opts;
              if (!(*opt_dec)->stdin_specified) {
                opts.close_stdin = true;
              }
            } else {
              opts.close_stdin = true;
            }

            std::vector<std::string> argv;
            argv.reserve(1 + args_dec->args.size());
            argv.push_back(std::move(*command));
            for (auto& a : args_dec->args) {
              argv.push_back(std::move(a));
            }

            // Capture for the promise start (no JS values).
            auto* pr = pr_capture;
            auto start =
                [pr,
                 argv = std::move(argv),
                 opts = std::move(opts)](
                    std::stop_token stop) mutable
                -> runtime::Task<proc::RunResult> {
              if (stop.stop_requested()) {
                co_return std::unexpected(cancelled_err("run"));
              }
              auto created = pr->create(std::move(argv), std::move(opts));
              if (!created) {
                co_return std::unexpected(
                    runtime::Error::from_domain(std::move(created.error())));
              }
              auto process = std::move(*created);

              auto bridge = make_process_stop_bridge(stop, process);

              auto started = map_proc_void(co_await process->start());
              if (!started) {
                (void)co_await process->async_close();
                if (stop.stop_requested()) {
                  co_return std::unexpected(cancelled_err("run"));
                }
                co_return std::unexpected(std::move(started.error()));
              }

              auto waited = map_proc_result(co_await process->wait());
              (void)co_await process->async_close();

              if (stop.stop_requested()) {
                co_return std::unexpected(cancelled_err("run"));
              }
              co_return waited;
            };

            JSValue promise = async->promise<proc::RunResult>(
                jc,
                std::move(start),
                [](JSContext* c, proc::RunResult&& r)
                    -> runtime::Result<qjs::OwnedValue> {
                  try {
                    qjs::OwnedValue owned =
                        binding::Converter<proc::RunResult>::to_js(
                            binding::Env{c}, std::move(r));
                    if (owned.is_exception()) {
                      if (JS_HasException(c)) {
                        JSValue ex = JS_GetException(c);
                        JS_FreeValue(c, ex);
                      }
                      (void)owned.release();
                      return std::unexpected(runtime::Error::native(
                          "failed to encode ProcessResult"));
                    }
                    return owned;
                  } catch (const std::bad_alloc&) {
                    throw;
                  } catch (const std::exception& ex) {
                    if (JS_HasException(c)) {
                      JSValue e = JS_GetException(c);
                      JS_FreeValue(c, e);
                    }
                    return std::unexpected(runtime::Error::native(ex.what()));
                  } catch (...) {
                    if (JS_HasException(c)) {
                      JSValue e = JS_GetException(c);
                      JS_FreeValue(c, e);
                    }
                    return std::unexpected(runtime::Error::native(
                        "failed to encode ProcessResult"));
                  }
                });
            return qjs::OwnedValue::take(jc, promise);
          },
          3);
      if (run_fn.is_exception()) {
        return -1;
      }
      if (mod.set_export(m, "run", std::move(run_fn)) != 0) {
        return -1;
      }
    }

    return 0;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return -1;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return -1;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "process module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_process(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_process);
    if (m == nullptr) {
      return nullptr;
    }
    for (const char* export_name : k_process_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "process module: declare_export failed");
        }
        return nullptr;
      }
    }
    return m;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return nullptr;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return nullptr;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "process module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
