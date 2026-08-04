#include "fs/file.hpp"

#include "fs/io_uring_probe.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#if defined(BOOST_ASIO_HAS_FILE)
#include <boost/asio/random_access_file.hpp>
#define VACPS_FS_HAS_ASIO_FILE 1
#else
#define VACPS_FS_HAS_ASIO_FILE 0
#endif

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vacps::fs {
namespace {

#if VACPS_FS_HAS_ASIO_FILE
using AsioFile = asio::random_access_file;
#endif

[[nodiscard]] FileStat file_stat_from_fd(
    int fd,
    const std::string& display_path) {
  FileStat out;
  out.path = display_path;
#if defined(__linux__)
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return out;
  }
  // fstat never returns S_IFLNK for a non-O_PATH fd of a symlink open target.
  out.is_symlink = false;
  if (S_ISDIR(st.st_mode)) {
    out.type = "directory";
  } else if (S_ISREG(st.st_mode)) {
    out.type = "file";
    out.size_bytes = static_cast<std::uint64_t>(st.st_size);
  } else {
    out.type = "other";
  }
  out.modified_at_ms = static_cast<std::int64_t>(st.st_mtime) * 1000;
  out.readable = (st.st_mode & S_IRUSR) != 0;
  out.writable = (st.st_mode & S_IWUSR) != 0;
#else
  (void)fd;
  out.type = "other";
#endif
  return out;
}

[[nodiscard]] bool compute_prefer_asio(FileBackend backend) {
  // Never construct Asio file objects unless the process probe succeeded.
  switch (backend) {
    case FileBackend::Posix:
      return false;
    case FileBackend::Asio:
    case FileBackend::Auto:
    default:
      return io_uring_available();
  }
}

/** Operation-local cancellation bridge (main-executor posts only). */
struct OpCancelState {
  asio::cancellation_signal signal;
  bool active{true};
};

}  // namespace

void File::PreparedOpen::reset() noexcept {
#if defined(__linux__)
  if (fd >= 0) {
    ::close(fd);
  }
#endif
  fd = -1;
}

struct File::PosixFd {
  explicit PosixFd(int fd) noexcept : fd_(fd) {}
  ~PosixFd() { reset(); }

  PosixFd(const PosixFd&) = delete;
  PosixFd& operator=(const PosixFd&) = delete;
  PosixFd(PosixFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  PosixFd& operator=(PosixFd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }

  void reset() noexcept {
#if defined(__linux__)
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
    fd_ = -1;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
  int fd_{-1};
};

/**
 * Asio backend state.
 *
 * Ownership (deliberate two-descriptor design):
 * - `file` owns the assigned random_access_file fd (io_uring data plane).
 *   All methods on `file` run only on `executor`.
 * - `control` is a F_DUPFD_CLOEXEC duplicate used exclusively for run_blocking
 *   blocking control/append (fstat/ftruncate/fsync/write under O_APPEND).
 *   Callers must externally serialize so control ops do not overlap the
 *   data plane (JS FileHandle owns FileOperationQueue for that).
 */
struct File::AsioState {
#if VACPS_FS_HAS_ASIO_FILE
  AsioState(asio::any_io_executor ex, int control_fd)
      : executor(std::move(ex)), file(executor), control(control_fd) {}
  asio::any_io_executor executor;
  AsioFile file;
  PosixFd control;
#else
  AsioState(asio::any_io_executor, int) {}
#endif
};

File::File(
    PrivateTag,
    std::unique_ptr<PosixFd> fd,
    std::string display_path,
    OpenMode open_mode)
    : use_asio_(false),
      posix_fd_(std::move(fd)),
      display_path_(std::move(display_path)),
      open_mode_(open_mode) {}

File::File(
    PrivateTag,
    std::unique_ptr<AsioState> asio_state,
    std::string display_path,
    OpenMode open_mode)
    : use_asio_(true),
      asio_(std::move(asio_state)),
      display_path_(std::move(display_path)),
      open_mode_(open_mode) {}

File::~File() {
  std::lock_guard lock(mu_);
  life_ = Life::Closed;
  close_handles();
}

bool File::offset_ok(std::uint64_t offset) noexcept {
  return offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

bool File::offset_add_ok(std::uint64_t base, std::uint64_t delta) noexcept {
  if (delta > (std::numeric_limits<std::uint64_t>::max)() - base) {
    return false;
  }
  return offset_ok(base + delta);
}

VoidResult File::check_range(
    std::uint64_t offset,
    std::size_t length,
    std::string_view op) {
  if (!offset_ok(offset)) {
    return std::unexpected(Error{
        std::format("{} offset out of range", op), std::string{op}, 0});
  }
  if (length == 0) {
    return {};
  }
  // Validate full span [offset, offset+length) before any I/O.
  if (!offset_add_ok(offset, static_cast<std::uint64_t>(length - 1))) {
    return std::unexpected(Error{
        std::format("{} range overflow", op),
        std::string{op},
        EOVERFLOW});
  }
  return {};
}

Result<std::shared_ptr<File>> File::adopt_posix(
    int fd,
    std::string display_path,
    OpenMode mode) {
  // Boundary 1: stack PosixFd ctor is noexcept — owns fd before any allocation.
  PosixFd guard{fd};
  try {
    // Boundary 2: new-expression allocates unique_ptr storage first; only then
    // evaluates PosixFd(guard.release()). If allocation throws, guard still
    // owns fd. After success, ownership moves exactly once into the unique_ptr.
    auto owner = std::make_unique<PosixFd>(guard.release());
    // Boundary 3: File storage allocation before ctor args; owner already RAII.
    return std::shared_ptr<File>(new File(
        PrivateTag{}, std::move(owner), std::move(display_path), mode));
  } catch (const std::bad_alloc&) {
    return std::unexpected(
        Error{"complete_open: allocation failed", "open", ENOMEM});
  } catch (...) {
    return std::unexpected(
        Error{"complete_open: failed to construct File", "open", 0});
  }
}

VoidResult File::advance_cursor(std::uint64_t delta) noexcept {
  if (!offset_add_ok(offset_, delta)) {
    return std::unexpected(
        Error{"cursor overflow", "io", EOVERFLOW});
  }
  offset_ += delta;
  return {};
}

Result<std::size_t> File::resolve_read_max(std::size_t max_bytes) {
  if (max_bytes == (std::numeric_limits<std::size_t>::max)()) {
    return kDefaultMaxReadBytes;
  }
  if (max_bytes > kHardMaxReadBytes) {
    return std::unexpected(Error{
        std::format(
            "read maxBytes exceeds hard limit ({} bytes / 64 MiB)",
            kHardMaxReadBytes),
        "read",
        0});
  }
  return max_bytes;
}

VoidResult File::validate_permissions(const OpenOptions& options) {
  if (!options.permissions.has_value()) {
    return {};
  }
  const auto bits = *options.permissions;
  if ((bits & ~static_cast<std::uint32_t>(0777)) != 0) {
    return std::unexpected(Error{
        std::format(
            "File.open: permissions 0{:o} out of range (only mode bits 0777 allowed)",
            bits),
        "open",
        0});
  }
  return {};
}

Result<File::PreparedOpen> File::prepare_open(
    std::string_view path,
    const OpenOptions& options,
    const std::filesystem::path& relative_base,
    FileBackend backend) {
  if (auto v = validate_permissions(options); !v) {
    return std::unexpected(std::move(v.error()));
  }
  auto abs = resolve_path(relative_base, path);
  if (!abs) {
    return std::unexpected(std::move(abs.error()));
  }

#if !defined(__linux__)
  (void)options;
  (void)backend;
  return std::unexpected(Error{"File::open requires Linux", "open", 0});
#else
  const int flags = posix_open_flags(options.mode) | O_CLOEXEC;
  const mode_t mode_bits = open_mode_creates(options.mode)
                               ? static_cast<mode_t>(effective_permissions(options))
                               : static_cast<mode_t>(0);
  const int raw = ::open(abs->c_str(), flags, mode_bits);
  if (raw < 0) {
    return std::unexpected(make_io_error("open", errno, abs->string()));
  }

  PreparedOpen out;
  out.fd = raw;
  out.display_path = abs->string();
  out.mode = options.mode;
  out.prefer_asio = compute_prefer_asio(backend);
  return out;
#endif
}

Result<std::shared_ptr<File>> File::complete_open(
    PreparedOpen prepared,
    asio::any_io_executor file_executor) {
  if (prepared.fd < 0) {
    return std::unexpected(Error{"complete_open: invalid fd", "open", 0});
  }

  const OpenMode mode = prepared.mode;
  std::string display = std::move(prepared.display_path);
  // Prefer Asio only when probe already succeeded (prefer_asio) and executor set.
  const bool want_asio =
      prepared.prefer_asio && static_cast<bool>(file_executor);

#if VACPS_FS_HAS_ASIO_FILE && defined(__linux__)
  if (want_asio) {
    // Exception-safe setup: allocate Asio state + control dup WHILE prepared
    // still owns the original fd. Never release without live RAII ownership.
    std::unique_ptr<AsioState> state;
    int control_raw = -1;
    try {
      control_raw = ::fcntl(prepared.fd, F_DUPFD_CLOEXEC, 0);
      if (control_raw < 0) {
        // Dup failed — POSIX fallback; prepared still owns original.
        const int fd = prepared.release();
        return adopt_posix(fd, std::move(display), mode);
      }
      // control_raw immediately owned by AsioState::control (PosixFd).
      state = std::make_unique<AsioState>(file_executor, control_raw);
      control_raw = -1;
    } catch (...) {
      if (control_raw >= 0) {
        ::close(control_raw);
      }
      // prepared still owns original fd → POSIX fallback via RAII helper.
      const int fd = prepared.release();
      return adopt_posix(fd, std::move(display), mode);
    }

    // AsioState + control established. Transfer original via assign.
    // Park data fd in a temporary PosixFd until assign takes ownership.
    PosixFd data_owner{prepared.release()};
    boost::system::error_code ec;
    const int data_fd = data_owner.get();
    state->file.assign(data_fd, ec);
    if (ec) {
      // assign failed: data_owner still owns data_fd; rebuild POSIX from control.
      const int posix = state->control.release();
      state.reset();
      data_owner.reset();  // close data fd
      if (posix >= 0) {
        return adopt_posix(posix, std::move(display), mode);
      }
      return std::unexpected(Error{
          std::format(
              "open failed: assign random_access_file: {} (code={})",
              ec.message(),
              ec.value()),
          "open",
          ec.value()});
    }
    // assign took ownership of data_fd — release without close.
    (void)data_owner.release();

    // Two descriptors live in state; adopt into File under RAII.
    try {
      return std::shared_ptr<File>(new File(
          PrivateTag{}, std::move(state), std::move(display), mode));
    } catch (const std::bad_alloc&) {
      state.reset();  // closes both descriptors
      return std::unexpected(
          Error{"complete_open: allocation failed", "open", ENOMEM});
    } catch (...) {
      state.reset();
      return std::unexpected(
          Error{"complete_open: failed to construct File", "open", 0});
    }
  }
#else
  (void)want_asio;
  (void)file_executor;
#endif

  const int fd = prepared.release();
  return adopt_posix(fd, std::move(display), mode);
}

Result<std::shared_ptr<File>> File::open(
    std::string_view path,
    const OpenOptions& options,
    const std::filesystem::path& relative_base,
    asio::any_io_executor file_executor,
    FileBackend backend) {
  auto prepared = prepare_open(path, options, relative_base, backend);
  if (!prepared) {
    return std::unexpected(std::move(prepared.error()));
  }
  if (prepared->prefer_asio && !file_executor) {
    prepared->prefer_asio = false;
  }
  return complete_open(std::move(*prepared), std::move(file_executor));
}

bool File::closed() const noexcept {
  std::lock_guard lock(mu_);
  return life_ == Life::Closed;
}

VoidResult File::ensure_open() const {
  if (life_ != Life::Open) {
    return std::unexpected(Error{"file is closed", "io", 0});
  }
  return {};
}

int File::control_fd() const noexcept {
  if (use_asio_) {
#if VACPS_FS_HAS_ASIO_FILE
    if (asio_ && asio_->control) {
      return asio_->control.get();
    }
#endif
    return -1;
  }
  if (posix_fd_ && *posix_fd_) {
    return posix_fd_->get();
  }
  return -1;
}

void File::close_handles() noexcept {
  if (use_asio_) {
#if VACPS_FS_HAS_ASIO_FILE
    if (asio_) {
      if (asio_->file.is_open()) {
        boost::system::error_code ec;
        // Supported path: explicit close on the owning executor after
        // external serialization (no concurrent op; JS FileHandle holds the
        // operation queue) and after async coroutines have released
        // keep_alive. ~File is best-effort RAII only once idle.
        asio_->file.cancel(ec);
        asio_->file.close(ec);
        (void)ec;
      }
      asio_->control.reset();
    }
#endif
    asio_.reset();
    use_asio_ = false;
  }
  posix_fd_.reset();
}

// ── Sync I/O on control/owned fd ──────────────────────────────────

Result<std::vector<std::uint8_t>> File::read_at(
    std::uint64_t offset,
    std::size_t max_bytes) {
  auto lim = resolve_read_max(max_bytes);
  if (!lim) {
    return std::unexpected(std::move(lim.error()));
  }
  max_bytes = *lim;

  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (max_bytes == 0) {
    return std::vector<std::uint8_t>{};
  }
  if (auto r = check_range(offset, max_bytes, "readAt"); !r) {
    return std::unexpected(std::move(r.error()));
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "readAt", 0});
  }

#if defined(__linux__)
  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  while (got < max_bytes) {
    const std::size_t want = std::min(kChunk, max_bytes - got);
    const ssize_t n = ::pread(
        fd, tmp.data(), want, static_cast<off_t>(offset + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(make_io_error("readAt", errno, display_path_));
    }
    if (n == 0) break;
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += static_cast<std::size_t>(n);
  }
  return buf;
#else
  return std::unexpected(Error{"readAt requires Linux", "readAt", 0});
#endif
}

Result<std::size_t> File::write_at(
    std::uint64_t offset,
    std::span<const std::uint8_t> data) {
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (is_append_mode()) {
    return std::unexpected(Error{
        "writeAt is not supported on append handles; use write() "
        "(O_APPEND write)",
        "writeAt",
        0});
  }
  if (data.empty()) {
    return std::size_t{0};
  }
  if (auto r = check_range(offset, data.size(), "writeAt"); !r) {
    return std::unexpected(std::move(r.error()));
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "writeAt", 0});
  }

#if defined(__linux__)
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::pwrite(
        fd,
        data.data() + off,
        data.size() - off,
        static_cast<off_t>(offset + off));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(make_io_error("writeAt", errno, display_path_));
    }
    if (n == 0) {
      return std::unexpected(
          Error{"writeAt failed: zero bytes written", "writeAt", 0});
    }
    off += static_cast<std::size_t>(n);
  }
  return off;
#else
  return std::unexpected(Error{"writeAt requires Linux", "writeAt", 0});
#endif
}

Result<std::vector<std::uint8_t>> File::read(std::size_t max_bytes) {
  auto lim = resolve_read_max(max_bytes);
  if (!lim) {
    return std::unexpected(std::move(lim.error()));
  }
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const std::uint64_t at = offset_;
  if (*lim == 0) {
    return std::vector<std::uint8_t>{};
  }
  if (auto r = check_range(at, *lim, "read"); !r) {
    return std::unexpected(std::move(r.error()));
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "read", 0});
  }
#if defined(__linux__)
  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  const std::size_t max_b = *lim;
  while (got < max_b) {
    if (!offset_add_ok(at, static_cast<std::uint64_t>(got))) {
      return std::unexpected(Error{"read offset overflow", "read", EOVERFLOW});
    }
    const std::size_t want = std::min(kChunk, max_b - got);
    const ssize_t n =
        ::pread(fd, tmp.data(), want, static_cast<off_t>(at + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(make_io_error("read", errno, display_path_));
    }
    if (n == 0) break;
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += static_cast<std::size_t>(n);
  }
  if (!offset_add_ok(at, static_cast<std::uint64_t>(got))) {
    return std::unexpected(Error{"cursor overflow", "read", EOVERFLOW});
  }
  offset_ = at + static_cast<std::uint64_t>(got);
  return buf;
#else
  return std::unexpected(Error{"read requires Linux", "read", 0});
#endif
}

Result<std::size_t> File::append_write(std::span<const std::uint8_t> data) {
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (data.empty()) {
    return std::size_t{0};
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "write", 0});
  }
#if defined(__linux__)
  // Each write(2) under O_APPEND is kernel-positioned atomically. Multiple
  // partial writes in this loop are not one indivisible append.
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(make_io_error("write", errno, display_path_));
    }
    if (n == 0) {
      return std::unexpected(
          Error{"write failed: zero bytes written", "write", 0});
    }
    off += static_cast<std::size_t>(n);
  }
  struct stat st{};
  if (::fstat(fd, &st) == 0) {
    offset_ = static_cast<std::uint64_t>(st.st_size);
  } else if (offset_add_ok(offset_, static_cast<std::uint64_t>(off))) {
    offset_ += static_cast<std::uint64_t>(off);
  }
  return off;
#else
  return std::unexpected(Error{"write requires Linux", "write", 0});
#endif
}

Result<std::size_t> File::write(std::span<const std::uint8_t> data) {
  if (is_append_mode()) {
    return append_write(data);
  }
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (data.empty()) {
    return std::size_t{0};
  }
  const std::uint64_t at = offset_;
  if (auto r = check_range(at, data.size(), "write"); !r) {
    return std::unexpected(std::move(r.error()));
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "write", 0});
  }
#if defined(__linux__)
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::pwrite(
        fd,
        data.data() + off,
        data.size() - off,
        static_cast<off_t>(at + off));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(make_io_error("write", errno, display_path_));
    }
    if (n == 0) {
      return std::unexpected(
          Error{"write failed: zero bytes written", "write", 0});
    }
    off += static_cast<std::size_t>(n);
  }
  if (!offset_add_ok(at, static_cast<std::uint64_t>(off))) {
    return std::unexpected(Error{"cursor overflow", "write", EOVERFLOW});
  }
  offset_ = at + static_cast<std::uint64_t>(off);
  return off;
#else
  return std::unexpected(Error{"write requires Linux", "write", 0});
#endif
}

VoidResult File::truncate(std::uint64_t size) {
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return ok;
  }
  if (!offset_ok(size)) {
    return std::unexpected(Error{"truncate size out of range", "truncate", 0});
  }
  // Control fd only — never Asio resize from a worker.
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "truncate", 0});
  }
#if defined(__linux__)
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    return std::unexpected(make_io_error("truncate", errno, display_path_));
  }
  // Cursor deliberately unchanged (may point past EOF).
  return {};
#else
  return std::unexpected(Error{"truncate requires Linux", "truncate", 0});
#endif
}

Result<FileStat> File::stat() {
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "stat", 0});
  }
#if defined(__linux__)
  FileStat out = file_stat_from_fd(fd, display_path_);
  if (out.type.empty()) {
    return std::unexpected(make_io_error("stat", errno, display_path_));
  }
  return out;
#else
  return std::unexpected(Error{"stat requires Linux", "stat", 0});
#endif
}

VoidResult File::flush() {
  std::lock_guard lock(mu_);
  if (auto ok = ensure_open(); !ok) {
    return ok;
  }
  const int fd = control_fd();
  if (fd < 0) {
    return std::unexpected(Error{"file is closed", "flush", 0});
  }
#if defined(__linux__)
  if (::fsync(fd) != 0) {
    return std::unexpected(make_io_error("flush", errno, display_path_));
  }
  return {};
#else
  return std::unexpected(Error{"flush requires Linux", "flush", 0});
#endif
}

VoidResult File::close() {
  std::lock_guard lock(mu_);
  if (life_ == Life::Closed || life_ == Life::Closing) {
    life_ = Life::Closed;
    return {};
  }
  life_ = Life::Closing;
  close_handles();
  life_ = Life::Closed;
  return {};
}

// ── Genuine async Asio / io_uring data path ───────────────────────
// Public entry points are ordinary functions (not coroutines): they capture
// shared_from_this() immediately and pass shared_ptr by value into static
// coroutine impls. Parameters are stored in the coroutine frame before the
// initial suspend, so File stays alive across call→first-resume and all awaits.

asio::awaitable<Result<std::vector<std::uint8_t>>> File::async_read_at(
    std::uint64_t offset,
    std::size_t max_bytes,
    std::stop_token stop) {
  return async_read_at_impl(shared_from_this(), offset, max_bytes, std::move(stop));
}

asio::awaitable<Result<std::size_t>> File::async_write_at(
    std::uint64_t offset,
    std::vector<std::uint8_t> data,
    std::stop_token stop) {
  return async_write_at_impl(
      shared_from_this(), offset, std::move(data), std::move(stop));
}

asio::awaitable<Result<std::vector<std::uint8_t>>> File::async_read_at_impl(
    std::shared_ptr<File> self,
    std::uint64_t offset,
    std::size_t max_bytes,
    std::stop_token stop) {
#if !VACPS_FS_HAS_ASIO_FILE
  (void)self;
  (void)offset;
  (void)max_bytes;
  (void)stop;
  co_return std::unexpected(
      Error{"Asio file support not compiled", "readAt", 0});
#else
  if (!self->use_asio_ || !self->asio_) {
    co_return std::unexpected(
        Error{"async_read_at requires Asio backend", "readAt", 0});
  }
  if (self->life_ != Life::Open) {
    co_return std::unexpected(Error{"file is closed", "readAt", 0});
  }
  auto lim = resolve_read_max(max_bytes);
  if (!lim) {
    co_return std::unexpected(std::move(lim.error()));
  }
  max_bytes = *lim;
  if (max_bytes == 0) {
    co_return std::vector<std::uint8_t>{};
  }
  if (auto r = check_range(offset, max_bytes, "readAt"); !r) {
    co_return std::unexpected(std::move(r.error()));
  }
  if (stop.stop_requested()) {
    co_return std::unexpected(
        Error{"operation cancelled", "readAt", ECANCELED});
  }

  // Operation-local cancel: stop_token → post to file executor → signal.
  // active flag prevents a delayed post from cancelling a later op.
  auto op = std::make_shared<OpCancelState>();
  auto weak_op = std::weak_ptr<OpCancelState>(op);
  auto ex = self->asio_->executor;
  std::stop_callback on_stop{
      stop, [weak_op, ex]() noexcept {
        try {
          asio::post(ex, [weak_op]() {
            if (auto s = weak_op.lock(); s && s->active) {
              s->signal.emit(asio::cancellation_type::all);
            }
          });
        } catch (...) {
        }
      }};

  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  while (got < max_bytes) {
    if (stop.stop_requested()) {
      op->active = false;
      co_return std::unexpected(
          Error{"operation cancelled", "readAt", ECANCELED});
    }
    const std::size_t want = std::min(kChunk, max_bytes - got);
    auto [ec, n] = co_await self->asio_->file.async_read_some_at(
        offset + got,
        asio::buffer(tmp.data(), want),
        asio::bind_cancellation_slot(
            op->signal.slot(), asio::as_tuple(asio::use_awaitable)));
    if (ec == asio::error::eof) {
      break;
    }
    if (ec == asio::error::operation_aborted || stop.stop_requested()) {
      op->active = false;
      co_return std::unexpected(
          Error{"operation cancelled", "readAt", ECANCELED});
    }
    if (ec) {
      op->active = false;
      co_return std::unexpected(Error{
          std::format(
              "readAt failed: {} (code={})", ec.message(), ec.value()),
          "readAt",
          ec.value()});
    }
    if (n == 0) {
      break;
    }
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += n;
  }
  op->active = false;
  co_return buf;
#endif
}

asio::awaitable<Result<std::size_t>> File::async_write_at_impl(
    std::shared_ptr<File> self,
    std::uint64_t offset,
    std::vector<std::uint8_t> data,
    std::stop_token stop) {
#if !VACPS_FS_HAS_ASIO_FILE
  (void)self;
  (void)offset;
  (void)data;
  (void)stop;
  co_return std::unexpected(
      Error{"Asio file support not compiled", "writeAt", 0});
#else
  if (!self->use_asio_ || !self->asio_) {
    co_return std::unexpected(
        Error{"async_write_at requires Asio backend", "writeAt", 0});
  }
  if (self->life_ != Life::Open) {
    co_return std::unexpected(Error{"file is closed", "writeAt", 0});
  }
  if (self->is_append_mode()) {
    co_return std::unexpected(Error{
        "writeAt is not supported on append handles; use write() "
        "(O_APPEND write)",
        "writeAt",
        0});
  }
  if (data.empty()) {
    co_return std::size_t{0};
  }
  if (auto r = check_range(offset, data.size(), "writeAt"); !r) {
    co_return std::unexpected(std::move(r.error()));
  }
  if (stop.stop_requested()) {
    co_return std::unexpected(
        Error{"operation cancelled", "writeAt", ECANCELED});
  }

  auto op = std::make_shared<OpCancelState>();
  auto weak_op = std::weak_ptr<OpCancelState>(op);
  auto ex = self->asio_->executor;
  std::stop_callback on_stop{
      stop, [weak_op, ex]() noexcept {
        try {
          asio::post(ex, [weak_op]() {
            if (auto s = weak_op.lock(); s && s->active) {
              s->signal.emit(asio::cancellation_type::all);
            }
          });
        } catch (...) {
        }
      }};

  std::size_t off = 0;
  while (off < data.size()) {
    if (stop.stop_requested()) {
      op->active = false;
      co_return std::unexpected(
          Error{"operation cancelled", "writeAt", ECANCELED});
    }
    auto [ec, n] = co_await self->asio_->file.async_write_some_at(
        offset + off,
        asio::buffer(data.data() + off, data.size() - off),
        asio::bind_cancellation_slot(
            op->signal.slot(), asio::as_tuple(asio::use_awaitable)));
    if (ec == asio::error::operation_aborted || stop.stop_requested()) {
      op->active = false;
      co_return std::unexpected(
          Error{"operation cancelled", "writeAt", ECANCELED});
    }
    if (ec) {
      op->active = false;
      co_return std::unexpected(Error{
          std::format(
              "writeAt failed: {} (code={})", ec.message(), ec.value()),
          "writeAt",
          ec.value()});
    }
    if (n == 0) {
      op->active = false;
      co_return std::unexpected(
          Error{"writeAt failed: zero bytes written", "writeAt", 0});
    }
    off += n;
  }
  op->active = false;
  co_return off;
#endif
}

}  // namespace vacps::fs
