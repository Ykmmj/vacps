#include "modules/bindings.hpp"

#include "binding/async_function.hpp"
#include "binding/class.hpp"
#include "binding/module.hpp"
#include "fs/file.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"
#include "modules/catalog.hpp"
#include "modules/detail/file_operation_queue.hpp"
#include "modules/fs_convert.hpp"
#include "runtime/runtime_async.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <quickjs.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
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
namespace asio = boost::asio;

constexpr const char* k_fs_exports[] = {
    "File",
    "mkdir",
    "remove",
    "rename",
    "stat",
    "exists",
    "readDirectory",
};

/**
 * Module-native handle: domain File + per-handle FileOperationQueue.
 * ClassBuilder opaque type. The queue serializes each complete method across
 * awaits; Runtime::Async remains the sole run_blocking / Promise owner. Domain
 * File does not own the queue and requires externally serialized access.
 */
class FileHandle : public std::enable_shared_from_this<FileHandle> {
 public:
  FileHandle(
      std::shared_ptr<vacps::fs::File> file,
      std::shared_ptr<detail::FileOperationQueue> operation_queue)
      : file_(std::move(file)),
        operation_queue_(std::move(operation_queue)) {}

  [[nodiscard]] const std::shared_ptr<vacps::fs::File>& file() const noexcept {
    return file_;
  }
  [[nodiscard]] const std::shared_ptr<detail::FileOperationQueue>&
  operation_queue() const noexcept {
    return operation_queue_;
  }

  [[nodiscard]] std::string path() const {
    return file_->display_path();
  }
  [[nodiscard]] std::string mode() const {
    return vacps::fs::open_mode_to_string(file_->open_mode());
  }
  [[nodiscard]] bool closed() const noexcept {
    return file_->closed();
  }

 private:
  std::shared_ptr<vacps::fs::File> file_;
  std::shared_ptr<detail::FileOperationQueue> operation_queue_;
};

template <class T>
[[nodiscard]] runtime::Result<T> map_fs_result(vacps::Result<T> r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return std::move(*r);
}

[[nodiscard]] runtime::VoidResult map_fs_void(vacps::VoidResult r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return {};
}

[[nodiscard]] runtime::Error cancelled_err(std::string_view op) {
  return runtime::Error::cancelled_op(std::string{op});
}

/** Acquire exclusive File operation lease; map operation_aborted → cancelled. */
[[nodiscard]] asio::awaitable<
    runtime::Result<detail::FileOperationQueue::Lease>>
acquire_operation_lease(
    const std::shared_ptr<detail::FileOperationQueue>& operation_queue,
    std::stop_token stop,
    std::string_view op) {
  auto [ec, lease] = co_await operation_queue->async_acquire(
      stop, asio::as_tuple(asio::use_awaitable));
  if (ec || !lease) {
    co_return std::unexpected(cancelled_err(op));
  }
  co_return std::move(lease);
}

/**
 * Module init (phase 2): File class + namespace path ops.
 * No C++ exception may escape this C callback.
 *
 * Runtime::Async and host data_dir are recovered from the mandatory JSRuntime
 * composition opaque.
 */
int initialize_fs(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder mod{env};

    // Snapshot host-wired data_dir at init (composition always installed).
    std::string data_dir = composition_from_context(ctx).data_dir;

    using FileBuilder = binding::ClassBuilder<FileHandle>;

    auto file_class =
        FileBuilder{env, "File"}
            .constructor(
                [](const binding::CallbackInfo&)
                    -> binding::Result<std::shared_ptr<FileHandle>> {
                  return std::unexpected(binding::Error::type(
                      "File cannot be constructed with new; "
                      "use File.open(path, options)"));
                },
                0)
            .static_async_function(
                "open",
                [async, data_dir](
                    std::stop_token stop,
                    std::string path,
                    vacps::fs::OpenOptions options) mutable
                    -> runtime::Task<std::shared_ptr<FileHandle>> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("open"));
                  }
                  // Phase 1: blocking resolve + open(2) on worker.
                  auto prepared = co_await async->run_blocking(
                      stop,
                      [path = std::move(path),
                       options = std::move(options),
                       data_dir]() mutable {
                        return map_fs_result(vacps::fs::File::prepare_open(
                            path, options, data_dir, vacps::fs::FileBackend::Auto));
                      });
                  if (!prepared) {
                    co_return std::unexpected(std::move(prepared.error()));
                  }
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("open"));
                  }
                  // Phase 2: main executor — assign into random_access_file
                  // when prefer_asio (data fd + control dup; two descriptors).
                  auto file = vacps::fs::File::complete_open(
                      std::move(*prepared), async->executor());
                  if (!file) {
                    co_return std::unexpected(
                        runtime::Error::from_domain(std::move(file.error())));
                  }
                  auto operation_queue =
                      std::make_shared<detail::FileOperationQueue>(
                          async->executor());
                  co_return std::make_shared<FileHandle>(
                      std::move(*file), std::move(operation_queue));
                },
                2)
            .readonly(
                "path",
                [](const FileHandle& self) -> std::string {
                  return self.path();
                })
            .readonly(
                "mode",
                [](const FileHandle& self) -> std::string {
                  return self.mode();
                })
            .readonly(
                "closed",
                [](const FileHandle& self) -> bool { return self.closed(); })
            .async_method(
                "read",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self,
                    std::optional<std::uint64_t> max_bytes) mutable
                    -> runtime::Task<std::vector<std::uint8_t>> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("read"));
                  }
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "read");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("read"));
                  }
                  const std::size_t req =
                      max_bytes.has_value()
                          ? static_cast<std::size_t>(*max_bytes)
                          : (std::numeric_limits<std::size_t>::max)();
                  auto lim = vacps::fs::File::resolve_read_max(req);
                  if (!lim) {
                    co_return map_fs_result(
                        vacps::Result<std::vector<std::uint8_t>>(
                            std::unexpected(std::move(lim.error()))));
                  }
                  auto& f = *self->file();
                  if (f.uses_asio_file()) {
                    const auto at = f.cursor();
                    auto data =
                        co_await f.async_read_at(at, *lim, stop);
                    if (data) {
                      if (auto adv = f.advance_cursor(
                              static_cast<std::uint64_t>(data->size()));
                          !adv) {
                        co_return std::unexpected(
                            runtime::Error::from_domain(adv.error()));
                      }
                    }
                    co_return map_fs_result(std::move(data));
                  }
                  co_return co_await async->run_blocking(
                      stop, [self, n = *lim]() mutable {
                        return map_fs_result(self->file()->read(n));
                      });
                },
                0)
            .async_method(
                "readAt",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self,
                    std::uint64_t offset,
                    std::uint64_t max_bytes) mutable
                    -> runtime::Task<std::vector<std::uint8_t>> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("readAt"));
                  }
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "readAt");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("readAt"));
                  }
                  auto lim = vacps::fs::File::resolve_read_max(
                      static_cast<std::size_t>(max_bytes));
                  if (!lim) {
                    co_return map_fs_result(
                        vacps::Result<std::vector<std::uint8_t>>(
                            std::unexpected(std::move(lim.error()))));
                  }
                  auto& f = *self->file();
                  if (f.uses_asio_file()) {
                    co_return map_fs_result(
                        co_await f.async_read_at(offset, *lim, stop));
                  }
                  co_return co_await async->run_blocking(
                      stop,
                      [self, offset, n = *lim]() mutable {
                        return map_fs_result(
                            self->file()->read_at(offset, n));
                      });
                },
                2)
            .async_method(
                "write",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self,
                    binding::fs_module::StrictBytes bytes) mutable
                    -> runtime::Task<std::size_t> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("write"));
                  }
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "write");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("write"));
                  }
                  auto& f = *self->file();
                  auto data = std::move(bytes.data);
                  // Append: O_APPEND write(2) via run_blocking on control fd.
                  if (f.is_append_mode()) {
                    co_return co_await async->run_blocking(
                        stop,
                        [self, data = std::move(data)]() mutable {
                          return map_fs_result(self->file()->append_write(
                              data));
                        });
                  }
                  if (f.uses_asio_file()) {
                    const auto at = f.cursor();
                    auto n = co_await f.async_write_at(
                        at, std::move(data), stop);
                    if (n) {
                      if (auto adv = f.advance_cursor(
                              static_cast<std::uint64_t>(*n));
                          !adv) {
                        co_return std::unexpected(
                            runtime::Error::from_domain(adv.error()));
                      }
                    }
                    co_return map_fs_result(std::move(n));
                  }
                  co_return co_await async->run_blocking(
                      stop,
                      [self, data = std::move(data)]() mutable {
                        return map_fs_result(self->file()->write(data));
                      });
                },
                1)
            .async_method(
                "writeAt",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self,
                    std::uint64_t offset,
                    binding::fs_module::StrictBytes bytes) mutable
                    -> runtime::Task<std::size_t> {
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("writeAt"));
                  }
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "writeAt");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("writeAt"));
                  }
                  auto& f = *self->file();
                  if (f.is_append_mode()) {
                    co_return std::unexpected(runtime::Error::native(
                        "writeAt is not supported on append handles; use "
                        "write() (kernel-atomic O_APPEND)"));
                  }
                  auto data = std::move(bytes.data);
                  if (f.uses_asio_file()) {
                    co_return map_fs_result(co_await f.async_write_at(
                        offset, std::move(data), stop));
                  }
                  co_return co_await async->run_blocking(
                      stop,
                      [self, offset, data = std::move(data)]() mutable {
                        return map_fs_result(
                            self->file()->write_at(offset, data));
                      });
                },
                2)
            .async_method(
                "truncate",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self,
                    std::uint64_t size) mutable -> runtime::Task<void> {
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "truncate");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("truncate"));
                  }
                  // Blocking control op — run_blocking while operation lease held.
                  co_return co_await async->run_blocking(
                      stop, [self, size]() mutable {
                        return map_fs_void(self->file()->truncate(size));
                      });
                },
                1)
            .async_method(
                "stat",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self) mutable
                    -> runtime::Task<vacps::fs::FileStat> {
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "stat");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("stat"));
                  }
                  co_return co_await async->run_blocking(
                      stop, [self]() mutable {
                        return map_fs_result(self->file()->stat());
                      });
                },
                0)
            .async_method(
                "flush",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self) mutable
                    -> runtime::Task<void> {
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "flush");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  if (stop.stop_requested()) {
                    co_return std::unexpected(cancelled_err("flush"));
                  }
                  co_return co_await async->run_blocking(
                      stop, [self]() mutable {
                        return map_fs_void(self->file()->flush());
                      });
                },
                0)
            .async_method(
                "close",
                [async](
                    std::stop_token stop,
                    std::shared_ptr<FileHandle> self) mutable
                    -> runtime::Task<void> {
                  // Queue-serialized: waits for any in-flight op, then close.
                  auto leased = co_await acquire_operation_lease(
                      self->operation_queue(), stop, "close");
                  if (!leased) {
                    co_return std::unexpected(std::move(leased.error()));
                  }
                  auto lease = std::move(*leased);
                  auto& f = *self->file();
                  if (f.uses_asio_file()) {
                    // Asio cancel/close on owning main executor (here).
                    co_return map_fs_void(f.close());
                  }
                  co_return co_await async->run_blocking(
                      stop, [self]() mutable {
                        return map_fs_void(self->file()->close());
                      });
                },
                0)
            .commit();

    if (!file_class) {
      (void)binding::throw_error(ctx, file_class.error());
      return -1;
    }
    if (mod.set_export(m, "File", std::move(*file_class)) != 0) {
      return -1;
    }

    // ── Namespace free functions (always run_blocking) ───────────────

    auto export_async = [&](const char* name, auto fn, int length) -> bool {
      qjs::OwnedValue func = binding::create_async_function(
          env, name, std::move(fn), length);
      return mod.set_export(m, name, std::move(func)) == 0;
    };

    if (!export_async(
            "mkdir",
            [async, data_dir](
                std::stop_token stop,
                std::string path,
                vacps::fs::MkdirOptions options) mutable
                -> runtime::Task<void> {
              co_return co_await async->run_blocking(
                  stop,
                  [path = std::move(path),
                   options,
                   data_dir]() mutable {
                    auto abs = vacps::fs::resolve_path(data_dir, path);
                    if (!abs) {
                      return map_fs_void(
                          vacps::VoidResult(std::unexpected(std::move(abs.error()))));
                    }
                    return map_fs_void(vacps::fs::mkdir(*abs, options));
                  });
            },
            1)) {
      return -1;
    }

    if (!export_async(
            "remove",
            [async, data_dir](
                std::stop_token stop,
                std::string path,
                vacps::fs::RemoveOptions options) mutable
                -> runtime::Task<void> {
              co_return co_await async->run_blocking(
                  stop,
                  [path = std::move(path),
                   options,
                   data_dir]() mutable {
                    auto abs = vacps::fs::resolve_path(data_dir, path);
                    if (!abs) {
                      return map_fs_void(
                          vacps::VoidResult(std::unexpected(std::move(abs.error()))));
                    }
                    return map_fs_void(
                        vacps::fs::remove_path(*abs, options));
                  });
            },
            1)) {
      return -1;
    }

    if (!export_async(
            "rename",
            [async, data_dir](
                std::stop_token stop,
                std::string from,
                std::string to,
                vacps::fs::RenameOptions options) mutable
                -> runtime::Task<void> {
              co_return co_await async->run_blocking(
                  stop,
                  [from = std::move(from),
                   to = std::move(to),
                   options,
                   data_dir]() mutable {
                    auto a = vacps::fs::resolve_path(data_dir, from);
                    if (!a) {
                      return map_fs_void(
                          vacps::VoidResult(std::unexpected(std::move(a.error()))));
                    }
                    auto b = vacps::fs::resolve_path(data_dir, to);
                    if (!b) {
                      return map_fs_void(
                          vacps::VoidResult(std::unexpected(std::move(b.error()))));
                    }
                    return map_fs_void(
                        vacps::fs::rename_path(*a, *b, options));
                  });
            },
            2)) {
      return -1;
    }

    if (!export_async(
            "stat",
            [async, data_dir](
                std::stop_token stop,
                std::string path) mutable
                -> runtime::Task<vacps::fs::FileStat> {
              co_return co_await async->run_blocking(
                  stop, [path = std::move(path), data_dir]() mutable {
                    auto abs = vacps::fs::resolve_path(data_dir, path);
                    if (!abs) {
                      return map_fs_result(
                          vacps::Result<vacps::fs::FileStat>(
                              std::unexpected(std::move(abs.error()))));
                    }
                    return map_fs_result(vacps::fs::file_stat(*abs));
                  });
            },
            1)) {
      return -1;
    }

    if (!export_async(
            "exists",
            [async, data_dir](
                std::stop_token stop,
                std::string path) mutable -> runtime::Task<bool> {
              co_return co_await async->run_blocking(
                  stop, [path = std::move(path), data_dir]() mutable {
                    auto abs = vacps::fs::resolve_path(data_dir, path);
                    if (!abs) {
                      return map_fs_result(vacps::Result<bool>(
                          std::unexpected(std::move(abs.error()))));
                    }
                    return map_fs_result(vacps::fs::exists(*abs));
                  });
            },
            1)) {
      return -1;
    }

    if (!export_async(
            "readDirectory",
            [async, data_dir](
                std::stop_token stop,
                std::string path) mutable
                -> runtime::Task<std::vector<vacps::fs::DirEntry>> {
              co_return co_await async->run_blocking(
                  stop, [path = std::move(path), data_dir]() mutable {
                    auto abs = vacps::fs::resolve_path(data_dir, path);
                    if (!abs) {
                      return map_fs_result(
                          vacps::Result<std::vector<vacps::fs::DirEntry>>(
                              std::unexpected(std::move(abs.error()))));
                    }
                    return map_fs_result(vacps::fs::list_dir(*abs));
                  });
            },
            1)) {
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
      (void)binding::throw_internal(ctx, "fs module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_fs(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_fs);
    if (m == nullptr) {
      return nullptr;
    }

    for (const char* export_name : k_fs_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "fs module: declare_export failed");
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
      (void)binding::throw_internal(ctx, "fs module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
