#include "fs/sandbox.hpp"

#include "fs/fs.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace vacps::fs {
namespace {

bool contains_null(std::string_view s) {
  return s.find('\0') != std::string_view::npos;
}

std::filesystem::path absolutize(const std::filesystem::path& p) {
  std::error_code ec;
  if (p.is_absolute()) {
    return p.lexically_normal();
  }
  auto abs = std::filesystem::absolute(p, ec);
  if (ec) return p.lexically_normal();
  return abs.lexically_normal();
}

/** realpath when path exists; otherwise absolute lexical form. */
std::filesystem::path real_or_abs(const std::filesystem::path& p) {
  std::error_code ec;
  auto canon = std::filesystem::weakly_canonical(p, ec);
  if (!ec) return canon.lexically_normal();
  return absolutize(p);
}

bool path_under_root(
    const std::filesystem::path& abs,
    const std::filesystem::path& root) {
  auto a = abs.lexically_normal().string();
  auto r = root.lexically_normal().string();
  if (r.size() > 1 && r.back() == '/') r.pop_back();
  if (a.size() > 1 && a.back() == '/') a.pop_back();
  if (a == r) return true;
  if (r == "/") return a.starts_with('/');
  return a.starts_with(r + "/");
}

std::string relative_under(
    const std::filesystem::path& abs,
    const std::filesystem::path& root) {
  auto a = abs.lexically_normal();
  auto r = root.lexically_normal();
  auto rel = a.lexically_relative(r);
  auto s = rel.generic_string();
  if (s == ".") return {};
  // openat2 rejects absolute and ".." at start when RESOLVE_BENEATH is set.
  if (s.starts_with("..")) return {};
  return s;
}

#if defined(__linux__)

struct Fd {
  int fd{-1};
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  Fd() = default;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      if (fd >= 0) ::close(fd);
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  explicit operator bool() const noexcept { return fd >= 0; }
};

int sys_openat2(int dirfd, const char* path, const open_how* how) {
  return static_cast<int>(::syscall(SYS_openat2, dirfd, path, how, sizeof(*how)));
}

Result<std::filesystem::path> fd_to_path(int fd) {
  char buf[4096];
  const auto link = std::format("/proc/self/fd/{}", fd);
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    return std::unexpected(
        Error{std::format("path sandbox: readlink failed: {}", std::strerror(errno))});
  }
  buf[n] = '\0';
  return std::filesystem::path{buf}.lexically_normal();
}

/**
 * openat2(dirfd, rel, O_PATH|RESOLVE_BENEATH). Empty rel → reopen dirfd path.
 * Returns canonical path via /proc/self/fd.
 */
Result<std::filesystem::path> openat2_beneath(
    int root_fd,
    std::string_view rel) {
  open_how how{};
  how.flags = static_cast<__u64>(O_PATH | O_CLOEXEC);
  how.mode = 0;
  how.resolve = RESOLVE_BENEATH;

  const char* path = rel.empty() ? "." : rel.data();
  // Ensure NUL-terminated for non-empty rel (string_view may not be).
  std::string rel_owned;
  if (!rel.empty()) {
    rel_owned.assign(rel);
    path = rel_owned.c_str();
  }

  const int fd = sys_openat2(root_fd, path, &how);
  if (fd < 0) {
    if (errno == ENOSYS) {
      return std::unexpected(Error{"path sandbox: openat2 not supported"});
    }
    if (errno == ENOENT) {
      return std::unexpected(Error{"path sandbox: path not found"});
    }
    if (errno == EXDEV || errno == ELOOP || errno == ENOENT || errno == ENOTDIR) {
      return std::unexpected(Error{std::format(
          "path sandbox: openat2 blocked escape or invalid path ({})",
          std::strerror(errno))});
    }
    return std::unexpected(Error{std::format(
        "path sandbox: openat2 failed: {}", std::strerror(errno))});
  }
  Fd holder;
  holder.fd = fd;
  return fd_to_path(fd);
}

Result<std::filesystem::path> authorize_openat2(
    const std::filesystem::path& abs,
    const std::filesystem::path& root) {
  Fd root_fd;
  root_fd.fd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (root_fd.fd < 0) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root {}: {}", root.string(), std::strerror(errno))});
  }

  const auto rel = relative_under(abs, root);
  if (!path_under_root(abs, root) && rel.empty() && abs != root) {
    return std::unexpected(Error{"path sandbox: path outside allowed roots"});
  }

  // Try full path first (existing file/dir).
  auto opened = openat2_beneath(root_fd.fd, rel);
  if (opened) {
    if (!path_under_root(*opened, root) || is_kernel_filesystem(*opened)) {
      return std::unexpected(Error{"path sandbox: resolved path outside allowlist"});
    }
    return *opened;
  }

  // Only ENOENT → authorize parent for create (write of a new leaf).
  if (opened.error().message.find("not found") == std::string::npos) {
    return std::unexpected(std::move(opened.error()));
  }

  auto parent = abs.parent_path();
  if (parent.empty()) parent = root;
  if (!path_under_root(parent, root) && parent != root) {
    return std::unexpected(Error{"path sandbox: parent outside allowed roots"});
  }
  const auto parent_rel = relative_under(parent, root);
  auto parent_real = openat2_beneath(root_fd.fd, parent_rel);
  if (!parent_real) {
    // Fall through to realpath fallback caller if openat2 unsupported.
    return std::unexpected(std::move(parent_real.error()));
  }
  if (!path_under_root(*parent_real, root) || is_kernel_filesystem(*parent_real)) {
    return std::unexpected(Error{"path sandbox: parent resolved outside allowlist"});
  }
  const auto base = abs.filename();
  if (base.empty() || base == "." || base == "..") {
    return std::unexpected(Error{"path sandbox: invalid path basename"});
  }
  return (*parent_real / base).lexically_normal();
}

#endif  // __linux__

Result<std::filesystem::path> authorize_realpath_fallback(
    const std::filesystem::path& abs,
    const std::filesystem::path& root) {
  std::error_code ec;
  if (std::filesystem::exists(abs, ec)) {
    auto real = std::filesystem::weakly_canonical(abs, ec);
    if (ec) {
      return std::unexpected(
          Error{std::format("path sandbox: realpath failed: {}", ec.message())});
    }
    real = real.lexically_normal();
    if (!path_under_root(real, root) || is_kernel_filesystem(real)) {
      return std::unexpected(Error{"path sandbox: realpath outside allowlist"});
    }
    return real;
  }
  // Non-existent: canonicalize parent.
  auto parent = abs.parent_path();
  if (parent.empty()) parent = root;
  auto parent_real = std::filesystem::weakly_canonical(parent, ec);
  if (ec) {
    // Parent may not exist either — require lexical under root only.
    if (!path_under_root(abs, root)) {
      return std::unexpected(Error{"path sandbox: path outside allowed roots"});
    }
    return abs.lexically_normal();
  }
  parent_real = parent_real.lexically_normal();
  if (!path_under_root(parent_real, root) || is_kernel_filesystem(parent_real)) {
    return std::unexpected(Error{"path sandbox: parent realpath outside allowlist"});
  }
  const auto base = abs.filename();
  if (base.empty() || base == "." || base == "..") {
    return std::unexpected(Error{"path sandbox: invalid path basename"});
  }
  return (parent_real / base).lexically_normal();
}

}  // namespace

bool is_kernel_filesystem(const std::filesystem::path& abs) noexcept {
  const auto s = abs.lexically_normal().string();
  for (const char* prefix : {"/proc", "/sys", "/dev"}) {
    if (s == prefix || s.starts_with(std::string(prefix) + "/")) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> fs_extra_roots_from_env() {
  const char* raw = std::getenv("VACPS_FS_ALLOWED_ROOTS");
  if (raw == nullptr || raw[0] == '\0') {
    raw = std::getenv("FS_ALLOWED_ROOTS");
  }
  if (raw == nullptr || raw[0] == '\0') return {};
  std::vector<std::string> out;
  std::string_view sv{raw};
  std::size_t start = 0;
  while (start <= sv.size()) {
    const auto pos = sv.find_first_of(":,\n", start);
    const auto end = pos == std::string_view::npos ? sv.size() : pos;
    auto part = sv.substr(start, end - start);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
      part.remove_prefix(1);
    }
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
      part.remove_suffix(1);
    }
    if (!part.empty() && part.front() == '/') {
      out.emplace_back(part);
    }
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

PathSandbox PathSandbox::create(
    const std::filesystem::path& data_dir,
    std::vector<std::string> extra_roots) {
  std::vector<std::filesystem::path> roots;
  auto add = [&](const std::filesystem::path& p) {
    if (p.empty()) return;
    auto r = real_or_abs(p);
    if (is_kernel_filesystem(r)) return;
    for (const auto& existing : roots) {
      if (existing == r) return;
    }
    // Prefer longer roots first for matching.
    roots.push_back(std::move(r));
  };

  add(data_dir.empty() ? std::filesystem::path{"data"} : data_dir);
  add("/tmp");
  for (const auto& e : extra_roots) {
    if (!e.empty()) add(e);
  }

  std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
    return a.string().size() > b.string().size();
  });
  return PathSandbox{std::move(roots)};
}

Result<std::filesystem::path> PathSandbox::authorize(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  if (user_path.empty()) {
    return std::unexpected(Error{"path is required"});
  }
  if (contains_null(user_path)) {
    return std::unexpected(Error{"path contains a null byte"});
  }
  if (roots_.empty()) {
    return std::unexpected(Error{"path sandbox: no allowed roots configured"});
  }

  auto joined = resolve_path(relative_base, user_path);
  if (!joined) return std::unexpected(std::move(joined.error()));
  auto abs = absolutize(*joined);
  return authorize_absolute(abs);
}

Result<std::filesystem::path> PathSandbox::authorize_absolute(
    const std::filesystem::path& abs) const {
  if (is_kernel_filesystem(abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }

  // Pick the longest matching lexical root first (roots_ sorted longest-first).
  const std::filesystem::path* matched = nullptr;
  for (const auto& root : roots_) {
    if (path_under_root(abs, root)) {
      matched = &root;
      break;
    }
  }
  if (matched == nullptr) {
    return std::unexpected(Error{std::format(
        "path outside allowed roots ({})",
        [&] {
          std::string s;
          for (std::size_t i = 0; i < roots_.size(); ++i) {
            if (i) s += ", ";
            s += roots_[i].string();
          }
          return s;
        }())});
  }

#if defined(__linux__)
  auto via_openat2 = authorize_openat2(abs, *matched);
  if (via_openat2) return via_openat2;
  // ENOSYS or transient: fall back to realpath (still better than lexical-only).
  if (via_openat2.error().message.find("not supported") != std::string::npos) {
    return authorize_realpath_fallback(abs, *matched);
  }
  // "path not found" → try create-path parent path inside authorize_openat2 already;
  // if still failing with not found on parent, fall back.
  if (via_openat2.error().message.find("not found") != std::string::npos) {
    return authorize_realpath_fallback(abs, *matched);
  }
  return via_openat2;
#else
  return authorize_realpath_fallback(abs, *matched);
#endif
}

}  // namespace vacps::fs
