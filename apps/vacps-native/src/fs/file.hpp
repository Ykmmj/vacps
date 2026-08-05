#pragma once

#include "app/error.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::fs {

namespace asio = boost::asio;

/**
 * Open file handle — pure domain content I/O for vacps:fs.
 *
 * ## Dual backend (one public API)
 *
 * Selected once at complete_open after the process-wide io_uring probe:
 *
 * 1. **Asio / io_uring data plane** — `boost::asio::random_access_file` on the
 *    owning (main) executor. Data path uses genuine
 *    `async_read_some_at` / `async_write_some_at`. Deliberate **two-descriptor**
 *    ownership: (a) the assigned data fd inside random_access_file, (b) a
 *    `F_DUPFD_CLOEXEC` control fd for run_blocking fstat/ftruncate/fsync/append
 *    write. Never call random_access_file methods (including native_handle,
 *    cancel, close, resize, sync_all) from a worker. Explicit close/cancel of
 *    the Asio object runs on the owning executor (module main + operation
 *    queue serialization).
 *
 * 2. **POSIX fallback** — single owned fd + blocking pread/pwrite/write/
 *    fstat/ftruncate/fsync/close. Binding uses Runtime::Async::run_blocking.
 *
 * Probe failure (including Docker seccomp EPERM/ENOSYS) selects POSIX before
 * any Asio file object is constructed. Parent directories are never created.
 *
 * Append: each `write(2)` syscall under O_APPEND is kernel-positioned
 * atomically; a logical call spanning multiple partial writes is not one
 * indivisible append. `write_at` is rejected on append handles.
 *
 * Path `stat` (namespace) uses lstat semantics. Handle `stat` uses fstat on
 * the control/owned fd (opened target, not the symlink itself).
 *
 * No QuickJS / Runtime::Async / worker executor / private run_blocking in this type.
 * This domain File does not own a serialization queue; callers must serialize
 * access externally. The vacps:fs JS module FileHandle owns a module-private
 * FileOperationQueue for that purpose.
 *
 * Lifetime: public async_read_at / async_write_at are non-coroutine wrappers
 * that call shared_from_this() immediately and pass shared_ptr by value into
 * private static coroutine impls. Coroutine parameters live in the frame before
 * initial suspend, covering the call-to-first-resume gap and every later
 * co_await. With external serialization, destruction happens only when no op is
 * in flight. Concurrent shared access without external serialization is
 * undefined. Explicit close on the owning executor is the supported shutdown
 * path; ~File is best-effort RAII once idle.
 */
class File final : public std::enable_shared_from_this<File> {
 public:
  static constexpr std::size_t kDefaultMaxReadBytes = 16ull * 1024 * 1024;
  static constexpr std::size_t kHardMaxReadBytes = 64ull * 1024 * 1024;

  struct PreparedOpen {
    int fd{-1};
    std::string display_path;
    OpenMode mode{OpenMode::read};
    bool prefer_asio{false};

    PreparedOpen() = default;
    PreparedOpen(const PreparedOpen&) = delete;
    PreparedOpen& operator=(const PreparedOpen&) = delete;
    PreparedOpen(PreparedOpen&& o) noexcept
        : fd(std::exchange(o.fd, -1)),
          display_path(std::move(o.display_path)),
          mode(o.mode),
          prefer_asio(o.prefer_asio) {}
    PreparedOpen& operator=(PreparedOpen&& o) noexcept {
      if (this != &o) {
        reset();
        fd = std::exchange(o.fd, -1);
        display_path = std::move(o.display_path);
        mode = o.mode;
        prefer_asio = o.prefer_asio;
      }
      return *this;
    }
    ~PreparedOpen() { reset(); }

    void reset() noexcept;
    [[nodiscard]] int release() noexcept { return std::exchange(fd, -1); }
  };

  [[nodiscard]] static Result<PreparedOpen> prepare_open(
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {},
      FileBackend backend = FileBackend::Auto);

  /**
   * Phase 2 on the file executor (main). Prefer_asio requires a live probe
   * and non-empty executor; otherwise POSIX. Exception-safe FD transfer.
   */
  [[nodiscard]] static Result<std::shared_ptr<File>> complete_open(
      PreparedOpen prepared,
      asio::any_io_executor file_executor = {});

  /** Test helper only. */
  [[nodiscard]] static Result<std::shared_ptr<File>> open(
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {},
      asio::any_io_executor file_executor = {},
      FileBackend backend = FileBackend::Auto);

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&&) = delete;
  File& operator=(File&&) = delete;
  ~File();

  // ── Asio data path (main executor only) ──────────────────────────
  // Non-coroutine wrappers: shared_from_this() runs at the call site before
  // any lazy awaitable is returned.

  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>>
  async_read_at(
      std::uint64_t offset,
      std::size_t max_bytes,
      std::stop_token stop = {});

  [[nodiscard]] asio::awaitable<Result<std::size_t>> async_write_at(
      std::uint64_t offset,
      std::vector<std::uint8_t> data,
      std::stop_token stop = {});

  // ── Sync primitives (POSIX fd / control fd; binding run_blocking) ────

  [[nodiscard]] Result<std::vector<std::uint8_t>> read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] Result<std::size_t> write_at(
      std::uint64_t offset,
      std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::vector<std::uint8_t>> read(
      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
  [[nodiscard]] Result<std::size_t> write(std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::size_t> append_write(
      std::span<const std::uint8_t> data);
  /**
   * Truncate. Does not move the logical cursor (matches common positioned
   * I/O semantics). Cursor may then point past EOF; subsequent sequential
   * reads return empty; writes extend as usual.
   */
  [[nodiscard]] VoidResult truncate(std::uint64_t size);
  /** fstat on the control/owned fd (opened target). */
  [[nodiscard]] Result<FileStat> stat();
  [[nodiscard]] VoidResult flush();
  /**
   * Idempotent. Asio path: must run on the file executor (closes random_access
   * file + control fd). POSIX path: closes owned fd (may be run_blocking).
   */
  [[nodiscard]] VoidResult close();

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] bool uses_asio_file() const noexcept { return use_asio_; }
  [[nodiscard]] bool is_append_mode() const noexcept {
    return open_mode_appends(open_mode_);
  }
  [[nodiscard]] const std::string& display_path() const noexcept {
    return display_path_;
  }
  [[nodiscard]] OpenMode open_mode() const noexcept { return open_mode_; }
  [[nodiscard]] std::uint64_t cursor() const noexcept { return offset_; }

  /** Checked cursor advance; returns error on overflow. */
  [[nodiscard]] VoidResult advance_cursor(std::uint64_t delta) noexcept;
  void set_cursor(std::uint64_t c) noexcept { offset_ = c; }

  [[nodiscard]] static Result<std::size_t> resolve_read_max(
      std::size_t max_bytes);

  /** Validate permissions bits (must be within 0777 when set). */
  [[nodiscard]] static VoidResult validate_permissions(
      const OpenOptions& options);

 private:
  enum class Life : std::uint8_t { Open = 0, Closing = 1, Closed = 2 };

  struct PrivateTag {};
  struct AsioState;
  struct PosixFd;

  File(
      PrivateTag,
      std::unique_ptr<PosixFd> fd,
      std::string display_path,
      OpenMode open_mode);
  File(
      PrivateTag,
      std::unique_ptr<AsioState> asio_state,
      std::string display_path,
      OpenMode open_mode);

  [[nodiscard]] VoidResult ensure_open() const;
  /** Control/owned POSIX fd for run_blocking ops (never Asio native_handle). */
  [[nodiscard]] int control_fd() const noexcept;
  void close_handles() noexcept;

  [[nodiscard]] static bool offset_ok(std::uint64_t offset) noexcept;
  [[nodiscard]] static bool offset_add_ok(
      std::uint64_t base,
      std::uint64_t delta) noexcept;
  /** Validate [offset, offset+length) fits uint64_t and off_t when length>0. */
  [[nodiscard]] static VoidResult check_range(
      std::uint64_t offset,
      std::size_t length,
      std::string_view op);

  /** Adopt raw fd into PosixFd RAII before any File allocation. */
  [[nodiscard]] static Result<std::shared_ptr<File>> adopt_posix(
      int fd,
      std::string display_path,
      OpenMode mode);

  /** Private static coroutines; `self` is a frame parameter (pre-suspend). */
  [[nodiscard]] static asio::awaitable<Result<std::vector<std::uint8_t>>>
  async_read_at_impl(
      std::shared_ptr<File> self,
      std::uint64_t offset,
      std::size_t max_bytes,
      std::stop_token stop);

  [[nodiscard]] static asio::awaitable<Result<std::size_t>> async_write_at_impl(
      std::shared_ptr<File> self,
      std::uint64_t offset,
      std::vector<std::uint8_t> data,
      std::stop_token stop);

  bool use_asio_{false};
  std::unique_ptr<PosixFd> posix_fd_;
  std::unique_ptr<AsioState> asio_;
  std::string display_path_;
  OpenMode open_mode_{OpenMode::read};
  std::uint64_t offset_{0};
  Life life_{Life::Open};
  mutable std::mutex mu_;
};

}  // namespace vacps::fs
