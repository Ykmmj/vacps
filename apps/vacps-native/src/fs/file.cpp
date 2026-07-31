#include "fs/file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#if defined(BOOST_ASIO_HAS_FILE)
#include <boost/asio/random_access_file.hpp>
#define VACPS_FS_HAS_ASIO_FILE 1
#else
#define VACPS_FS_HAS_ASIO_FILE 0
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vacps::fs {
namespace {

#if VACPS_FS_HAS_ASIO_FILE
using AsioFile = asio::random_access_file;
#endif

/**
 * Pool-backend open(2) bits. Flags values already match POSIX open flags on
 * Linux; add close-on-exec. Not part of the public C++ API.
 */
[[nodiscard]] int flags_to_posix(Flags flags) noexcept {
  int bits = flags_to_int(flags);
#if defined(__linux__)
  bits |= O_CLOEXEC;
#if !defined(BOOST_ASIO_HAS_FILE)
  static_assert(static_cast<int>(Flags::read_only) == O_RDONLY);
  static_assert(static_cast<int>(Flags::write_only) == O_WRONLY);
  static_assert(static_cast<int>(Flags::read_write) == O_RDWR);
  static_assert(static_cast<int>(Flags::append) == O_APPEND);
  static_assert(static_cast<int>(Flags::create) == O_CREAT);
  static_assert(static_cast<int>(Flags::exclusive) == O_EXCL);
  static_assert(static_cast<int>(Flags::truncate) == O_TRUNC);
#endif
#endif
  return bits;
}

[[nodiscard]] FileStat file_stat_from_fd(int fd, const std::string& display_path) {
  FileStat out;
  out.path = display_path;
#if defined(__linux__)
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return out;  // empty type → caller maps to fstat error
  }
  out.is_symlink = S_ISLNK(st.st_mode);
  if (S_ISDIR(st.st_mode)) {
    out.type = "directory";
  } else if (S_ISLNK(st.st_mode)) {
    out.type = "symlink";
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

}  // namespace

struct File::AsioState {
#if VACPS_FS_HAS_ASIO_FILE
  explicit AsioState(asio::any_io_executor ex) : file(std::move(ex)) {}
  AsioFile file;
#else
  explicit AsioState(asio::any_io_executor) {}
#endif
};


/** Pool-backend FD RAII — private to File (not a product surface). */
struct File::PoolFd {
  explicit PoolFd(int fd) noexcept : fd_(fd) {}
  ~PoolFd() { reset(); }

  PoolFd(const PoolFd&) = delete;
  PoolFd& operator=(const PoolFd&) = delete;
  PoolFd(PoolFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  PoolFd& operator=(PoolFd&& o) noexcept {
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
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
  int fd_{-1};
};

File::File(
    PrivateTag,
    std::unique_ptr<PoolFd> fd,
    asio::thread_pool* pool,
    std::string display_path,
    Flags flags)
    : use_asio_(false),
      pool_fd_(std::move(fd)),
      pool_(pool),
      display_path_(std::move(display_path)),
      flags_(flags) {}

File::File(
    PrivateTag,
    std::unique_ptr<AsioState> asio_state,
    asio::thread_pool* pool,
    std::string display_path,
    Flags flags)
    : use_asio_(true),
      asio_(std::move(asio_state)),
      pool_(pool),
      display_path_(std::move(display_path)),
      flags_(flags) {}

File::~File() {
  (void)close();
}

bool File::flags_append() const noexcept {
#if defined(BOOST_ASIO_HAS_FILE)
  const auto append_bit = static_cast<unsigned>(asio::file_base::append);
#else
  const auto append_bit = static_cast<unsigned>(Flags::append);
#endif
  return (static_cast<unsigned>(flags_) & append_bit) != 0;
}

Result<std::shared_ptr<File>> File::open(
    asio::any_io_executor ex,
    asio::thread_pool* pool_fallback,
    bool use_asio_file,
    std::string_view path,
    const OpenOptions& options,
    const std::filesystem::path& relative_base) {
  auto abs = resolve_path(relative_base, path);
  if (!abs) {
    return std::unexpected(std::move(abs.error()));
  }

  if (flags_create(options.flags) && abs->has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(abs->parent_path(), ec);
    // EEXIST is fine; other create_directories errors surface on open.
  }

  // Dual backend is intentional product design (see File class comment).
  // Prefer Asio when the process probed io_uring; always keep pool as the
  // portable path. Do not collapse this to “POSIX-only” in review rewrites.

  // ── Backend A: Asio random_access_file (probe + executor) ───────
  // Never construct Asio file objects when use_asio_file is false (Docker
  // seccomp / no io_uring). open() path errors return immediately; only a
  // thrown system_error (late EPERM on io_uring) falls through to pool.
  // Note: Asio open does not take OpenOptions::mode (Boost API).
#if VACPS_FS_HAS_ASIO_FILE
  if (use_asio_file && ex) {
    try {
      auto state = std::make_unique<AsioState>(ex);
      boost::system::error_code ec;
      state->file.open(abs->string(), options.flags, ec);
      if (ec) {
        return std::unexpected(
            Error{std::format("open failed: {}", ec.message())});
      }
      // shared_ptr(new …): ctor is private; make_shared cannot access it.
      return std::shared_ptr<File>(new File(
          PrivateTag{},
          std::move(state),
          pool_fallback,
          abs->string(),
          options.flags));
    } catch (const boost::system::system_error&) {
      // Fall through to pool backend.
    }
  }
#else
  (void)ex;
  (void)use_asio_file;
#endif

  // ── Backend B: private FD + thread_pool offload ─────────────────
  // Always available on Linux; used when probe failed or Asio open threw.
  // Applies OpenOptions::mode on create (unlike Asio open).
#if defined(__linux__)
  const int posix_flags = flags_to_posix(options.flags);
  const mode_t bits =
      flags_create(options.flags) ? static_cast<mode_t>(options.mode) : static_cast<mode_t>(0);
  const int raw = ::open(abs->c_str(), posix_flags, bits);
  if (raw < 0) {
    return std::unexpected(
        Error{std::format("open failed: {}", std::strerror(errno))});
  }
  return std::shared_ptr<File>(new File(
      PrivateTag{},
      std::make_unique<PoolFd>(raw),
      pool_fallback,
      abs->string(),
      options.flags));
#else
  (void)options;
  (void)pool_fallback;
  return std::unexpected(Error{"File::open requires Linux"});
#endif
}

asio::awaitable<Result<std::shared_ptr<File>>> File::async_open(
    AsyncOptions opts,
    std::string path,
    OpenOptions options,
    std::filesystem::path relative_base) {
  // Asio backend must construct on the ioc executor (handle affinity).
  if (opts.use_asio_file && opts.executor) {
    co_return File::open(
        opts.executor,
        &opts.pool,
        true,
        path,
        options,
        relative_base);
  }
  // Pool backend: blocking open off the ioc.
  co_return co_await async_offload(
      opts.pool,
      [path = std::move(path),
       options,
       relative_base = std::move(relative_base),
       pool = &opts.pool] {
        return File::open(
            asio::any_io_executor{}, pool, false, path, options, relative_base);
      });
}

VoidResult File::ensure_open() const {
  if (closed()) {
    return std::unexpected(Error{"file is closed"});
  }
  return {};
}

bool File::closed() const noexcept {
  if (use_asio_) {
#if VACPS_FS_HAS_ASIO_FILE
    return !asio_ || !asio_->file.is_open();
#else
    return true;
#endif
  }
  return !pool_fd_ || !(*pool_fd_);
}

Result<std::uint64_t> File::current_size() const {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
#if VACPS_FS_HAS_ASIO_FILE
  if (use_asio_) {
    boost::system::error_code ec;
    const auto n = asio_->file.size(ec);
    if (ec) {
      return std::unexpected(Error{std::format("size failed: {}", ec.message())});
    }
    return static_cast<std::uint64_t>(n);
  }
#endif
#if defined(__linux__)
  struct stat st{};
  if (::fstat(pool_fd_->get(), &st) != 0) {
    return std::unexpected(
        Error{std::format("fstat failed: {}", std::strerror(errno))});
  }
  return static_cast<std::uint64_t>(st.st_size);
#else
  return std::unexpected(Error{"stat requires Linux"});
#endif
}

// ── Pool-backend sync I/O ─────────────────────────────────────────

Result<std::vector<std::uint8_t>> File::pool_read(std::size_t max_bytes) {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (max_bytes == 0) {
    return std::vector<std::uint8_t>{};
  }
  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  while (got < max_bytes) {
    const std::size_t want = std::min(kChunk, max_bytes - got);
    const ssize_t n = ::read(pool_fd_->get(), tmp.data(), want);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("read failed: {}", std::strerror(errno))});
    }
    if (n == 0) break;
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += static_cast<std::size_t>(n);
    offset_ += static_cast<std::uint64_t>(n);
  }
  return buf;
}

Result<std::vector<std::uint8_t>> File::pool_read_at(
    std::uint64_t offset,
    std::size_t max_bytes) {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (max_bytes == 0) {
    return std::vector<std::uint8_t>{};
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected(Error{"read_at offset out of range"});
  }
  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  while (got < max_bytes) {
    const std::size_t want = std::min(kChunk, max_bytes - got);
    const ssize_t n = ::pread(
        pool_fd_->get(),
        tmp.data(),
        want,
        static_cast<off_t>(offset + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("pread failed: {}", std::strerror(errno))});
    }
    if (n == 0) break;
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += static_cast<std::size_t>(n);
  }
  return buf;
}

Result<std::size_t> File::pool_write(std::span<const std::uint8_t> data) {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(pool_fd_->get(), data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("write failed: {}", std::strerror(errno))});
    }
    off += static_cast<std::size_t>(n);
    offset_ += static_cast<std::uint64_t>(n);
  }
  return off;
}

Result<std::size_t> File::pool_write_at(
    std::uint64_t offset,
    std::span<const std::uint8_t> data) {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected(Error{"write_at offset out of range"});
  }
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::pwrite(
        pool_fd_->get(),
        data.data() + off,
        data.size() - off,
        static_cast<off_t>(offset + off));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("pwrite failed: {}", std::strerror(errno))});
    }
    off += static_cast<std::size_t>(n);
  }
  return off;
}

Result<std::vector<std::uint8_t>> File::read(std::size_t max_bytes) {
  if (use_asio_) {
    return std::unexpected(
        Error{"File::read sync is pool-backend only; use async_read"});
  }
  return pool_read(max_bytes);
}

Result<std::vector<std::uint8_t>> File::read_at(
    std::uint64_t offset,
    std::size_t max_bytes) {
  if (use_asio_) {
    return std::unexpected(
        Error{"File::read_at sync is pool-backend only; use async_read_at"});
  }
  return pool_read_at(offset, max_bytes);
}

Result<std::size_t> File::write(std::span<const std::uint8_t> data) {
  if (use_asio_) {
    return std::unexpected(
        Error{"File::write sync is pool-backend only; use async_write"});
  }
  return pool_write(data);
}

Result<std::size_t> File::write_at(
    std::uint64_t offset,
    std::span<const std::uint8_t> data) {
  if (use_asio_) {
    return std::unexpected(
        Error{"File::write_at sync is pool-backend only; use async_write_at"});
  }
  return pool_write_at(offset, data);
}

Result<std::string> File::read_text(std::size_t max_bytes) {
  auto bytes = read(max_bytes);
  if (!bytes) {
    return std::unexpected(std::move(bytes.error()));
  }
  return std::string(bytes->begin(), bytes->end());
}

Result<std::size_t> File::write_text(std::string_view data) {
  return write(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

VoidResult File::truncate(std::uint64_t size) {
  if (auto ok = ensure_open(); !ok) {
    return ok;
  }
  if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected(Error{"truncate size out of range"});
  }
#if VACPS_FS_HAS_ASIO_FILE
  if (use_asio_) {
    boost::system::error_code ec;
    asio_->file.resize(size, ec);
    if (ec) {
      return std::unexpected(
          Error{std::format("truncate failed: {}", ec.message())});
    }
    return {};
  }
#endif
  if (::ftruncate(pool_fd_->get(), static_cast<off_t>(size)) != 0) {
    return std::unexpected(
        Error{std::format("truncate failed: {}", std::strerror(errno))});
  }
  return {};
}

Result<FileStat> File::stat() {
  if (auto ok = ensure_open(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
#if VACPS_FS_HAS_ASIO_FILE
  if (use_asio_) {
#if defined(__linux__)
    const int fd = asio_->file.native_handle();
    FileStat out = file_stat_from_fd(fd, display_path_);
    if (out.type.empty()) {
      return std::unexpected(
          Error{std::format("fstat failed: {}", std::strerror(errno))});
    }
    return out;
#else
    return std::unexpected(Error{"stat requires Linux"});
#endif
  }
#endif
#if defined(__linux__)
  FileStat out = file_stat_from_fd(pool_fd_->get(), display_path_);
  if (out.type.empty()) {
    return std::unexpected(
        Error{std::format("fstat failed: {}", std::strerror(errno))});
  }
  return out;
#else
  return std::unexpected(Error{"stat requires Linux"});
#endif
}

VoidResult File::flush() {
  if (auto ok = ensure_open(); !ok) {
    return ok;
  }
#if VACPS_FS_HAS_ASIO_FILE
  if (use_asio_) {
    boost::system::error_code ec;
    asio_->file.sync_all(ec);
    if (ec) {
      return std::unexpected(Error{std::format("fsync failed: {}", ec.message())});
    }
    return {};
  }
#endif
  if (::fsync(pool_fd_->get()) != 0) {
    return std::unexpected(
        Error{std::format("fsync failed: {}", std::strerror(errno))});
  }
  return {};
}

VoidResult File::close() {
  if (use_asio_) {
#if VACPS_FS_HAS_ASIO_FILE
    if (asio_ && asio_->file.is_open()) {
      boost::system::error_code ec;
      asio_->file.close(ec);
      (void)ec;
    }
#endif
    asio_.reset();
    use_asio_ = false;
    return {};
  }
  if (!pool_fd_) {
    return {};
  }
  pool_fd_.reset();
  return {};
}

// ── Asio-backend async I/O ────────────────────────────────────────

#if VACPS_FS_HAS_ASIO_FILE

asio::awaitable<Result<std::vector<std::uint8_t>>> File::asio_read_at(
    std::uint64_t offset,
    std::size_t max_bytes) {
  if (auto ok = ensure_open(); !ok) {
    co_return std::unexpected(std::move(ok.error()));
  }
  if (max_bytes == 0) {
    co_return std::vector<std::uint8_t>{};
  }
  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<std::uint8_t> buf;
  std::array<std::uint8_t, kChunk> tmp{};
  std::size_t got = 0;
  while (got < max_bytes) {
    const std::size_t want = std::min(kChunk, max_bytes - got);
    auto [ec, n] = co_await asio_->file.async_read_some_at(
        offset + got,
        asio::buffer(tmp.data(), want),
        asio::as_tuple(asio::use_awaitable));
    if (ec == asio::error::eof) {
      break;
    }
    if (ec) {
      co_return std::unexpected(
          Error{std::format("read failed: {}", ec.message())});
    }
    if (n == 0) break;
    buf.insert(buf.end(), tmp.data(), tmp.data() + n);
    got += n;
  }
  co_return buf;
}

asio::awaitable<Result<std::vector<std::uint8_t>>> File::asio_read(
    std::size_t max_bytes) {
  auto data = co_await asio_read_at(offset_, max_bytes);
  if (data) {
    offset_ += static_cast<std::uint64_t>(data->size());
  }
  co_return data;
}

asio::awaitable<Result<std::size_t>> File::asio_write_at(
    std::uint64_t offset,
    std::vector<std::uint8_t> data) {
  if (auto ok = ensure_open(); !ok) {
    co_return std::unexpected(std::move(ok.error()));
  }
  if (data.empty()) {
    co_return std::size_t{0};
  }
  std::size_t off = 0;
  while (off < data.size()) {
    auto [ec, n] = co_await asio_->file.async_write_some_at(
        offset + off,
        asio::buffer(data.data() + off, data.size() - off),
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      co_return std::unexpected(
          Error{std::format("write failed: {}", ec.message())});
    }
    if (n == 0) {
      co_return std::unexpected(Error{"write failed: zero bytes written"});
    }
    off += n;
  }
  co_return off;
}

asio::awaitable<Result<std::size_t>> File::asio_write(std::vector<std::uint8_t> data) {
  std::uint64_t at = offset_;
  if (flags_append()) {
    auto sz = current_size();
    if (!sz) {
      co_return std::unexpected(std::move(sz.error()));
    }
    at = *sz;
  }
  auto n = co_await asio_write_at(at, std::move(data));
  if (n) {
    offset_ = at + static_cast<std::uint64_t>(*n);
  }
  co_return n;
}

#else  // !VACPS_FS_HAS_ASIO_FILE

asio::awaitable<Result<std::vector<std::uint8_t>>> File::asio_read(std::size_t) {
  co_return std::unexpected(Error{"Asio file support not compiled"});
}
asio::awaitable<Result<std::vector<std::uint8_t>>> File::asio_read_at(
    std::uint64_t,
    std::size_t) {
  co_return std::unexpected(Error{"Asio file support not compiled"});
}
asio::awaitable<Result<std::size_t>> File::asio_write(std::vector<std::uint8_t>) {
  co_return std::unexpected(Error{"Asio file support not compiled"});
}
asio::awaitable<Result<std::size_t>> File::asio_write_at(
    std::uint64_t,
    std::vector<std::uint8_t>) {
  co_return std::unexpected(Error{"Asio file support not compiled"});
}

#endif

// ── Public async dispatch ─────────────────────────────────────────

asio::awaitable<Result<std::vector<std::uint8_t>>> File::async_read(
    std::size_t max_bytes) {
  if (use_asio_) {
    co_return co_await asio_read(max_bytes);
  }
  if (pool_ == nullptr) {
    co_return read(max_bytes);
  }
  co_return co_await async_offload(
      *pool_, [this, max_bytes] { return pool_read(max_bytes); });
}

asio::awaitable<Result<std::vector<std::uint8_t>>> File::async_read_at(
    std::uint64_t offset,
    std::size_t max_bytes) {
  if (use_asio_) {
    co_return co_await asio_read_at(offset, max_bytes);
  }
  if (pool_ == nullptr) {
    co_return read_at(offset, max_bytes);
  }
  co_return co_await async_offload(
      *pool_, [this, offset, max_bytes] { return pool_read_at(offset, max_bytes); });
}

asio::awaitable<Result<std::string>> File::async_read_text(std::size_t max_bytes) {
  auto bytes = co_await async_read(max_bytes);
  if (!bytes) {
    co_return std::unexpected(std::move(bytes.error()));
  }
  co_return std::string(bytes->begin(), bytes->end());
}

asio::awaitable<Result<std::size_t>> File::async_write(
    std::span<const std::uint8_t> data) {
  std::vector<std::uint8_t> owned(data.begin(), data.end());
  if (use_asio_) {
    co_return co_await asio_write(std::move(owned));
  }
  if (pool_ == nullptr) {
    co_return pool_write(std::span<const std::uint8_t>(owned.data(), owned.size()));
  }
  co_return co_await async_offload(
      *pool_, [this, owned = std::move(owned)] {
        return pool_write(std::span<const std::uint8_t>(owned.data(), owned.size()));
      });
}

asio::awaitable<Result<std::size_t>> File::async_write_at(
    std::uint64_t offset,
    std::span<const std::uint8_t> data) {
  std::vector<std::uint8_t> owned(data.begin(), data.end());
  if (use_asio_) {
    co_return co_await asio_write_at(offset, std::move(owned));
  }
  if (pool_ == nullptr) {
    co_return pool_write_at(
        offset, std::span<const std::uint8_t>(owned.data(), owned.size()));
  }
  co_return co_await async_offload(
      *pool_, [this, offset, owned = std::move(owned)] {
        return pool_write_at(
            offset, std::span<const std::uint8_t>(owned.data(), owned.size()));
      });
}

asio::awaitable<Result<std::size_t>> File::async_write_text(std::string data) {
  co_return co_await async_write(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

asio::awaitable<VoidResult> File::async_truncate(std::uint64_t size) {
  if (use_asio_) {
    co_return truncate(size);
  }
  if (pool_ == nullptr) {
    co_return truncate(size);
  }
  co_return co_await async_offload(*pool_, [this, size] { return truncate(size); });
}

asio::awaitable<Result<FileStat>> File::async_stat() {
  if (use_asio_) {
    co_return stat();
  }
  if (pool_ == nullptr) {
    co_return stat();
  }
  co_return co_await async_offload(*pool_, [this] { return stat(); });
}

asio::awaitable<VoidResult> File::async_flush() {
  if (use_asio_) {
    co_return flush();
  }
  if (pool_ == nullptr) {
    co_return flush();
  }
  co_return co_await async_offload(*pool_, [this] { return flush(); });
}

asio::awaitable<VoidResult> File::async_close() {
  if (use_asio_) {
    co_return close();
  }
  if (pool_ == nullptr) {
    co_return close();
  }
  co_return co_await async_offload(*pool_, [this] { return close(); });
}

}  // namespace vacps::fs
