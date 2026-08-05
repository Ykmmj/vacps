#include "modules/bindings.hpp"

#include "binding/class.hpp"
#include "binding/module.hpp"
#include "modules/catalog.hpp"
#include "modules/store_convert.hpp"
#include "runtime/runtime_async.hpp"
#include "storage/store.hpp"

#include <quickjs.h>

#include <exception>
#include <memory>
#include <new>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;
namespace store_module = vacps::js::store_module;

/** Named ES exports — declare at module create, set during init. */
constexpr const char* k_store_exports[] = {
    "Store",
};

/**
 * Map domain Result → runtime::Result inside worker lambdas.
 * Never return vacps::Result from run_blocking (run_blocking only understands
 * runtime::Error; a domain Result would nest as a plain value).
 */
template <class T>
[[nodiscard]] runtime::Result<T> map_store_result(vacps::Result<T> r) {
  if (!r) {
    return std::unexpected(
        runtime::Error::native(std::move(r.error().message)));
  }
  return std::move(*r);
}

[[nodiscard]] runtime::VoidResult map_store_void(vacps::VoidResult r) {
  if (!r) {
    return std::unexpected(
        runtime::Error::native(std::move(r.error().message)));
  }
  return {};
}

/**
 * Module-private JS resource owner.
 *
 * ClassHolder and async frames share this object. It uniquely owns the domain
 * Store and one per-instance worker strand, so complete SQLite operations are
 * FIFO without making the storage domain depend on Runtime.
 */
class StoreNative final {
 public:
  StoreNative(
      std::unique_ptr<storage::Store> store,
      Runtime::Async::SerialWorker worker) noexcept
      : store_(std::move(store)), worker_(std::move(worker)) {}

  StoreNative(const StoreNative&) = delete;
  StoreNative& operator=(const StoreNative&) = delete;
  StoreNative(StoreNative&&) = delete;
  StoreNative& operator=(StoreNative&&) = delete;

  [[nodiscard]] storage::Store& store() noexcept { return *store_; }
  [[nodiscard]] const storage::Store& store() const noexcept { return *store_; }

  [[nodiscard]] const Runtime::Async::SerialWorker& worker() const noexcept {
    return worker_;
  }

 private:
  std::unique_ptr<storage::Store> store_;
  Runtime::Async::SerialWorker worker_;
};

/**
 * Module init (phase 2): ClassBuilder commit + JS_SetModuleExport("Store").
 * No C++ exception may escape this C callback.
 *
 * Runtime::Async is recovered from the mandatory JSRuntime composition
 * opaque.
 */
int initialize_store(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder mod{env};

    // Capture non-owning Runtime::Async* (stable for Runtime lifetime).
    // Do not capture JS handles. Runtime::Async is the sole Promise owner.

    using StoreBuilder = binding::ClassBuilder<StoreNative>;

    auto committed =
        StoreBuilder{env, "Store"}
            .constructor(
                [](const binding::CallbackInfo&)
                    -> binding::Result<std::shared_ptr<StoreNative>> {
                  return std::unexpected(binding::Error::type(
                      "Store cannot be constructed with new; "
                      "use Store.open(path, options?)"));
                },
                0)
            .static_async_function(
                "open",
                [async](
                    std::stop_token stop,
                    std::string path,
                    storage::OpenOptions options) mutable
                    -> runtime::Task<std::shared_ptr<StoreNative>> {
                  // Decode already finished; run_blocking pure C++ only.
                  auto opened = co_await async->run_blocking(
                      stop,
                      [path = std::move(path),
                       options = std::move(options)]() mutable {
                        return map_store_result(
                            storage::Store::open(
                                std::move(path), std::move(options)));
                      });
                  if (!opened) {
                    co_return std::unexpected(std::move(opened.error()));
                  }
                  co_return std::make_shared<StoreNative>(
                      std::move(*opened), async->make_serial_worker());
                },
                1)
            .readonly(
                "path",
                [](const StoreNative& self) -> std::string {
                  // By value: copy out of the native instance.
                  return self.store().path();
                })
            .readonly(
                "closed",
                [](const StoreNative& self) -> bool {
                  return self.store().closed();
                })
            .async_method(
                "exec",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<StoreNative> self,
                    std::string sql) mutable -> runtime::Task<void> {
                  auto worker = self->worker();
                  co_return co_await async->run_blocking(
                      worker,
                      stop,
                      [self = std::move(self),
                       sql = std::move(sql)]() mutable {
                        return map_store_void(self->store().exec(sql));
                      });
                },
                1)
            .async_method(
                "run",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<StoreNative> self,
                    std::string sql,
                    std::vector<storage::SqlValue> params) mutable
                    -> runtime::Task<storage::RunResult> {
                  auto worker = self->worker();
                  co_return co_await async->run_blocking(
                      worker,
                      stop,
                      [self = std::move(self),
                       sql = std::move(sql),
                       params = std::move(params)]() mutable {
                        return map_store_result(
                            self->store().run(sql, params));
                      });
                },
                1)
            .async_method(
                "query",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<StoreNative> self,
                    std::string sql,
                    std::vector<storage::SqlValue> params,
                    store_module::QueryOptions options) mutable
                    -> runtime::Task<storage::QueryResult> {
                  // Fixed shape only: query(sql, params?, options?).
                  // Missing trailing args arrive as JS_UNDEFINED and are
                  // defaulted by Converter (no query(sql, options) path).
                  auto worker = self->worker();
                  co_return co_await async->run_blocking(
                      worker,
                      stop,
                      [self = std::move(self),
                       sql = std::move(sql),
                       params = std::move(params),
                       options = std::move(options)]() mutable {
                        return map_store_result(self->store().query(
                            sql,
                            params,
                            options.max_rows,
                            options.max_bytes));
                      });
                },
                1)
            .async_method(
                "transaction",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<StoreNative> self,
                    std::vector<storage::TransactionStep> steps) mutable
                    -> runtime::Task<
                        std::vector<storage::TransactionResult>> {
                  auto worker = self->worker();
                  co_return co_await async->run_blocking(
                      worker,
                      stop,
                      [self = std::move(self),
                       steps = std::move(steps)]() mutable {
                        return map_store_result(
                            self->store().transaction(steps));
                      });
                },
                1)
            .async_method(
                "close",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<StoreNative> self) mutable
                    -> runtime::Task<void> {
                  auto worker = self->worker();
                  co_return co_await async->run_blocking(
                      worker,
                      stop,
                      [self = std::move(self)]() mutable {
                        return map_store_void(self->store().close());
                      });
                },
                0)
            .commit();

    if (!committed) {
      (void)binding::throw_error(ctx, committed.error());
      return -1;
    }
    if (mod.set_export(m, "Store", std::move(*committed)) != 0) {
      return -1;
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
      (void)binding::throw_internal(ctx, "store module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_store(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_store);
    if (m == nullptr) {
      return nullptr;
    }

    // Phase 1: declare every named export (required before SetModuleExport).
    for (const char* export_name : k_store_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "store module: declare_export failed");
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
      (void)binding::throw_internal(ctx, "store module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
