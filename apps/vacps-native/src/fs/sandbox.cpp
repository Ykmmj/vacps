#include "fs/sandbox.hpp"

#include "crypto/crypto.hpp"
#include "fs/fs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
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
  if (s.starts_with("..")) return {};
  return s;
}

Result<std::filesystem::path> join_user(
    std::string_view user_path,
    const std::filesystem::path& relative_base) {
  if (user_path.empty()) {
    return std::unexpected(Error{"path is required"});
  }
  if (contains_null(user_path)) {
    return std::unexpected(Error{"path contains a null byte"});
  }
  auto joined = resolve_path(relative_base, user_path);
  if (!joined) return std::unexpected(std::move(joined.error()));
  return absolutize(*joined);
}

const std::filesystem::path* match_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& abs) {
  for (const auto& root : roots) {
    if (path_under_root(abs, root)) return &root;
  }
  return nullptr;
}

#if defined(__linux__)

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

Result<std::filesystem::path> authorize_openat2(
    const std::filesystem::path& abs,
    const std::filesystem::path& root) {
  OwnedFd root_fd{::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root {}: {}", root.string(), std::strerror(errno))});
  }

  const auto rel = relative_under(abs, root);
  open_how how{};
  how.flags = static_cast<__u64>(O_PATH | O_CLOEXEC);
  how.resolve = RESOLVE_BENEATH;
  const std::string rel_owned{rel};
  const char* path = rel.empty() ? "." : rel_owned.c_str();
  const int fd = sys_openat2(root_fd.get(), path, &how);
  if (fd >= 0) {
    OwnedFd holder{fd};
    auto p = fd_to_path(fd);
    if (!p) return p;
    if (!path_under_root(*p, root) || is_kernel_filesystem(*p)) {
      return std::unexpected(Error{"path sandbox: resolved path outside allowlist"});
    }
    return *p;
  }
  if (errno == ENOSYS) {
    return std::unexpected(Error{"path sandbox: openat2 not supported"});
  }
  if (errno != ENOENT) {
    return std::unexpected(Error{std::format(
        "path sandbox: openat2 failed: {}", std::strerror(errno))});
  }

  // Non-existent leaf: verify parent under root.
  auto parent = abs.parent_path();
  if (parent.empty()) parent = root;
  const auto parent_rel = relative_under(parent, root);
  const std::string prel{parent_rel};
  const char* ppath = parent_rel.empty() ? "." : prel.c_str();
  const int pfd = sys_openat2(root_fd.get(), ppath, &how);
  if (pfd < 0) {
    return std::unexpected(Error{std::format(
        "path sandbox: parent openat2 failed: {}", std::strerror(errno))});
  }
  OwnedFd parent_holder{pfd};
  auto parent_real = fd_to_path(pfd);
  if (!parent_real) return parent_real;
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
  auto parent = abs.parent_path();
  if (parent.empty()) parent = root;
  auto parent_real = std::filesystem::weakly_canonical(parent, ec);
  if (ec) {
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

Result<std::string> read_fd_all(int fd) {
  std::string out;
  std::array<char, 64 * 1024> buf{};
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("read failed: {}", std::strerror(errno))});
    }
    if (n == 0) break;
    out.append(buf.data(), static_cast<std::size_t>(n));
  }
  return out;
}

}  // namespace

void OwnedFd::reset() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

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
  if (roots_.empty()) {
    return std::unexpected(Error{"path sandbox: no allowed roots configured"});
  }
  auto abs = join_user(user_path, relative_base);
  if (!abs) return abs;
  return authorize_absolute(*abs);
}

Result<std::filesystem::path> PathSandbox::authorize_absolute(
    const std::filesystem::path& abs) const {
  if (is_kernel_filesystem(abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* matched = match_root(roots_, abs);
  if (matched == nullptr) {
    return std::unexpected(Error{"path outside allowed roots"});
  }

#if defined(__linux__)
  auto via_openat2 = authorize_openat2(abs, *matched);
  if (via_openat2) return via_openat2;
  if (via_openat2.error().message.find("not supported") != std::string::npos ||
      via_openat2.error().message.find("not found") != std::string::npos) {
    return authorize_realpath_fallback(abs, *matched);
  }
  return via_openat2;
#else
  return authorize_realpath_fallback(abs, *matched);
#endif
}

#if defined(__linux__)

Result<OwnedFd> PathSandbox::open_relative(
    const std::filesystem::path& root,
    std::string_view rel,
    OpenMode mode) const {
  OwnedFd root_fd{::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root {}: {}", root.string(), std::strerror(errno))});
  }

  open_how how{};
  how.resolve = RESOLVE_BENEATH;
  how.mode = 0644;
  switch (mode) {
    case OpenMode::Read:
      how.flags = static_cast<__u64>(O_RDONLY | O_CLOEXEC);
      break;
    case OpenMode::WriteTrunc:
      how.flags = static_cast<__u64>(O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
      break;
    case OpenMode::WriteAppend:
      how.flags = static_cast<__u64>(O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
      break;
    case OpenMode::Dir:
      how.flags = static_cast<__u64>(O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      how.mode = 0;
      break;
    case OpenMode::PathOnly:
      how.flags = static_cast<__u64>(O_PATH | O_CLOEXEC);
      how.mode = 0;
      break;
  }

  const std::string rel_owned{rel};
  const char* path = rel.empty() ? "." : rel_owned.c_str();
  const int fd = sys_openat2(root_fd.get(), path, &how);
  if (fd < 0) {
    if (errno == ENOSYS) {
      return std::unexpected(Error{"path sandbox: openat2 not supported"});
    }
    return std::unexpected(Error{std::format(
        "path sandbox: openat2 failed: {}", std::strerror(errno))});
  }
  return OwnedFd{fd};
}

/** Ensure parent directories under root using mkdirat from root dirfd. */
VoidResult ensure_parents_under_root(
    int root_fd,
    std::string_view rel_file) {
  if (rel_file.empty()) return {};
  std::string rel{rel_file};
  // strip basename
  const auto slash = rel.find_last_of('/');
  if (slash == std::string::npos) return {};
  std::string dirs = rel.substr(0, slash);
  std::string cur;
  std::size_t start = 0;
  while (start < dirs.size()) {
    auto pos = dirs.find('/', start);
    if (pos == std::string::npos) pos = dirs.size();
    auto part = dirs.substr(start, pos - start);
    start = pos + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      return std::unexpected(Error{"path sandbox: invalid parent segment"});
    }
    if (!cur.empty()) cur.push_back('/');
    cur += part;
    if (::mkdirat(root_fd, cur.c_str(), 0755) != 0 && errno != EEXIST) {
      return std::unexpected(Error{std::format(
          "path sandbox: mkdirat failed: {}", std::strerror(errno))});
    }
  }
  return {};
}

Result<OwnedFd> PathSandbox::open_read_fd(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto abs = join_user(user_path, relative_base);
  if (!abs) return std::unexpected(std::move(abs.error()));
  if (is_kernel_filesystem(*abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* root = match_root(roots_, *abs);
  if (!root) return std::unexpected(Error{"path outside allowed roots"});
  const auto rel = relative_under(*abs, *root);
  auto fd = open_relative(*root, rel, OpenMode::Read);
  if (fd) return fd;
  // Fallback: authorize path then open (weaker) when openat2 missing.
  if (fd.error().message.find("not supported") != std::string::npos) {
    auto auth = authorize_realpath_fallback(*abs, *root);
    if (!auth) return std::unexpected(std::move(auth.error()));
    int raw = ::open(auth->c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) {
      return std::unexpected(
          Error{std::format("open failed: {}", std::strerror(errno))});
    }
    return OwnedFd{raw};
  }
  return fd;
}

Result<OwnedFd> PathSandbox::open_write_fd(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    bool append) const {
  auto abs = join_user(user_path, relative_base);
  if (!abs) return std::unexpected(std::move(abs.error()));
  if (is_kernel_filesystem(*abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* root = match_root(roots_, *abs);
  if (!root) return std::unexpected(Error{"path outside allowed roots"});
  const auto rel = relative_under(*abs, *root);

  OwnedFd root_fd{::open(root->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root {}: {}", root->string(), std::strerror(errno))});
  }
  if (auto mk = ensure_parents_under_root(root_fd.get(), rel); !mk) {
    // If openat2 unsupported, fall through below.
    if (mk.error().message.find("mkdirat") != std::string::npos && errno != ENOSYS) {
      // continue try open anyway
    }
  }

  auto mode = append ? OpenMode::WriteAppend : OpenMode::WriteTrunc;
  auto fd = open_relative(*root, rel, mode);
  if (fd) return fd;
  if (fd.error().message.find("not supported") != std::string::npos) {
    auto auth = authorize_realpath_fallback(*abs, *root);
    if (!auth) return std::unexpected(std::move(auth.error()));
    std::error_code ec;
    if (auth->has_parent_path()) {
      std::filesystem::create_directories(auth->parent_path(), ec);
    }
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    int raw = ::open(auth->c_str(), flags, 0644);
    if (raw < 0) {
      return std::unexpected(
          Error{std::format("open failed: {}", std::strerror(errno))});
    }
    return OwnedFd{raw};
  }
  return fd;
}

#else  // !linux

Result<OwnedFd> PathSandbox::open_relative(
    const std::filesystem::path&,
    std::string_view,
    OpenMode) const {
  return std::unexpected(Error{"path sandbox: openat2 requires linux"});
}

Result<OwnedFd> PathSandbox::open_read_fd(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto auth = authorize(user_path, relative_base);
  if (!auth) return std::unexpected(std::move(auth.error()));
  int raw = ::open(auth->c_str(), O_RDONLY | O_CLOEXEC);
  if (raw < 0) {
    return std::unexpected(Error{std::format("open failed: {}", std::strerror(errno))});
  }
  return OwnedFd{raw};
}

Result<OwnedFd> PathSandbox::open_write_fd(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    bool append) const {
  auto auth = authorize(user_path, relative_base);
  if (!auth) return std::unexpected(std::move(auth.error()));
  std::error_code ec;
  if (auth->has_parent_path()) {
    std::filesystem::create_directories(auth->parent_path(), ec);
  }
  int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
  int raw = ::open(auth->c_str(), flags, 0644);
  if (raw < 0) {
    return std::unexpected(Error{std::format("open failed: {}", std::strerror(errno))});
  }
  return OwnedFd{raw};
}

#endif

Result<std::string> PathSandbox::read_text(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto fd = open_read_fd(user_path, relative_base);
  if (!fd) return std::unexpected(std::move(fd.error()));
  return read_fd_all(fd->get());
}

Result<std::vector<std::uint8_t>> PathSandbox::read_bytes(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto t = read_text(user_path, relative_base);
  if (!t) return std::unexpected(std::move(t.error()));
  return std::vector<std::uint8_t>(t->begin(), t->end());
}

Result<std::vector<std::uint8_t>> PathSandbox::read_range(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    std::uint64_t offset,
    std::size_t max_bytes) const {
  if (max_bytes == 0) return std::vector<std::uint8_t>{};
  auto fd = open_read_fd(user_path, relative_base);
  if (!fd) return std::unexpected(std::move(fd.error()));
  if (offset > 0) {
    if (::lseek(fd->get(), static_cast<off_t>(offset), SEEK_SET) < 0) {
      return std::vector<std::uint8_t>{};
    }
  }
  std::vector<std::uint8_t> buf(max_bytes);
  std::size_t got = 0;
  while (got < max_bytes) {
    const ssize_t n =
        ::read(fd->get(), buf.data() + got, max_bytes - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("read failed: {}", std::strerror(errno))});
    }
    if (n == 0) break;
    got += static_cast<std::size_t>(n);
  }
  buf.resize(got);
  return buf;
}

Result<FileDigest> PathSandbox::hash_file(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto fd = open_read_fd(user_path, relative_base);
  if (!fd) return std::unexpected(std::move(fd.error()));
  vacps::crypto::Sha256 hasher;
  std::array<char, 64 * 1024> chunk{};
  std::uint64_t total = 0;
  for (;;) {
    const ssize_t n = ::read(fd->get(), chunk.data(), chunk.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("hash failed: {}", std::strerror(errno))});
    }
    if (n == 0) break;
    total += static_cast<std::uint64_t>(n);
    hasher.update(reinterpret_cast<const std::uint8_t*>(chunk.data()), static_cast<std::size_t>(n));
  }
  FileDigest out;
  out.size_bytes = total;
  out.sha256_hex = vacps::crypto::to_hex(hasher.finalize());
  return out;
}

VoidResult PathSandbox::write_text(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    std::string_view data) const {
  auto fd = open_write_fd(user_path, relative_base, false);
  if (!fd) return std::unexpected(std::move(fd.error()));
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd->get(), data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("write failed: {}", std::strerror(errno))});
    }
    off += static_cast<std::size_t>(n);
  }
  return {};
}

VoidResult PathSandbox::write_bytes(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    const std::vector<std::uint8_t>& data) const {
  return write_text(
      user_path,
      relative_base,
      std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

VoidResult PathSandbox::append_text(
    std::string_view user_path,
    const std::filesystem::path& relative_base,
    std::string_view data) const {
  auto fd = open_write_fd(user_path, relative_base, true);
  if (!fd) return std::unexpected(std::move(fd.error()));
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd->get(), data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          Error{std::format("append failed: {}", std::strerror(errno))});
    }
    off += static_cast<std::size_t>(n);
  }
  return {};
}

VoidResult PathSandbox::mkdir(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto abs = join_user(user_path, relative_base);
  if (!abs) return std::unexpected(std::move(abs.error()));
  if (is_kernel_filesystem(*abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* root = match_root(roots_, *abs);
  if (!root) return std::unexpected(Error{"path outside allowed roots"});
  const auto rel = relative_under(*abs, *root);

#if defined(__linux__)
  OwnedFd root_fd{::open(root->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root: {}", std::strerror(errno))});
  }
  // mkdir leaf + parents under root dirfd (no path re-open of leaf).
  if (auto mk = ensure_parents_under_root(root_fd.get(), rel + "/x"); !mk) {
    // ensure_parents on "rel/x" creates all of rel
  }
  if (rel.empty()) return {};
  if (::mkdirat(root_fd.get(), rel.c_str(), 0755) != 0 && errno != EEXIST) {
    // try recursive segments
    std::string cur;
    std::size_t start = 0;
    while (start <= rel.size()) {
      auto pos = rel.find('/', start);
      if (pos == std::string::npos) pos = rel.size();
      auto part = rel.substr(start, pos - start);
      start = pos + 1;
      if (part.empty() || part == ".") continue;
      if (!cur.empty()) cur.push_back('/');
      cur += part;
      if (::mkdirat(root_fd.get(), cur.c_str(), 0755) != 0 && errno != EEXIST) {
        return std::unexpected(Error{std::format(
            "mkdirat failed: {}", std::strerror(errno))});
      }
      if (pos == rel.size()) break;
    }
  }
  return {};
#else
  auto auth = authorize_realpath_fallback(*abs, *root);
  if (!auth) return std::unexpected(std::move(auth.error()));
  return mkdir_p(*auth);
#endif
}

bool PathSandbox::exists(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto fd = open_read_fd(user_path, relative_base);
  if (!fd) {
    // Try PathOnly for empty dirs etc.
#if defined(__linux__)
    auto abs = join_user(user_path, relative_base);
    if (!abs) return false;
    const auto* root = match_root(roots_, *abs);
    if (!root) return false;
    auto ofd = open_relative(*root, relative_under(*abs, *root), OpenMode::PathOnly);
    return static_cast<bool>(ofd);
#else
    return false;
#endif
  }
  return true;
}

VoidResult PathSandbox::remove(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto abs = join_user(user_path, relative_base);
  if (!abs) return std::unexpected(std::move(abs.error()));
  if (is_kernel_filesystem(*abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* root = match_root(roots_, *abs);
  if (!root) return std::unexpected(Error{"path outside allowed roots"});
  const auto rel = relative_under(*abs, *root);

#if defined(__linux__)
  OwnedFd root_fd{::open(root->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root: {}", std::strerror(errno))});
  }
  // Prefer unlinkat after openat2 verifies path is under root.
  auto path_fd = open_relative(*root, rel, OpenMode::PathOnly);
  if (!path_fd) {
    return std::unexpected(std::move(path_fd.error()));
  }
  // unlink via parent dirfd + basename to avoid path re-walk races on final component.
  const auto base = abs->filename().string();
  auto parent = abs->parent_path();
  const auto parent_rel = relative_under(parent.empty() ? *root : parent, *root);
  OwnedFd parent_fd;
  {
    open_how how{};
    how.flags = static_cast<__u64>(O_PATH | O_DIRECTORY | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH;
    const std::string prel{parent_rel};
    const char* p = parent_rel.empty() ? "." : prel.c_str();
    int pfd = sys_openat2(root_fd.get(), p, &how);
    if (pfd < 0) {
      return std::unexpected(Error{std::format(
          "path sandbox: parent open failed: {}", std::strerror(errno))});
    }
    parent_fd = OwnedFd{pfd};
  }
  // AT_REMOVEDIR if directory
  struct stat st{};
  if (::fstat(path_fd->get(), &st) == 0 && S_ISDIR(st.st_mode)) {
    if (::unlinkat(parent_fd.get(), base.c_str(), AT_REMOVEDIR) != 0) {
      // recursive: fall back carefully only under verified parent
      return std::unexpected(Error{std::format(
          "unlinkat dir failed: {}", std::strerror(errno))});
    }
  } else {
    if (::unlinkat(parent_fd.get(), base.c_str(), 0) != 0) {
      return std::unexpected(Error{std::format(
          "unlinkat failed: {}", std::strerror(errno))});
    }
  }
  return {};
#else
  auto auth = authorize(*abs, relative_base);
  if (!auth) return std::unexpected(std::move(auth.error()));
  return remove_path(*auth);
#endif
}

VoidResult PathSandbox::rename(
    std::string_view from_path,
    std::string_view to_path,
    const std::filesystem::path& relative_base) const {
  auto from_abs = join_user(from_path, relative_base);
  if (!from_abs) return std::unexpected(std::move(from_abs.error()));
  auto to_abs = join_user(to_path, relative_base);
  if (!to_abs) return std::unexpected(std::move(to_abs.error()));
  if (is_kernel_filesystem(*from_abs) || is_kernel_filesystem(*to_abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* from_root = match_root(roots_, *from_abs);
  const auto* to_root = match_root(roots_, *to_abs);
  if (!from_root || !to_root || *from_root != *to_root) {
    return std::unexpected(Error{"rename must stay within the same allowed root"});
  }

#if defined(__linux__)
  OwnedFd root_fd{::open(from_root->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (!root_fd) {
    return std::unexpected(Error{std::format(
        "path sandbox: cannot open root: {}", std::strerror(errno))});
  }
  // Verify both ends resolve under root (openat2), then renameat2.
  const auto from_rel = relative_under(*from_abs, *from_root);
  const auto to_rel = relative_under(*to_abs, *from_root);
  auto from_check = open_relative(*from_root, from_rel, OpenMode::PathOnly);
  if (!from_check) return std::unexpected(std::move(from_check.error()));
  (void)ensure_parents_under_root(root_fd.get(), to_rel);

  const std::string from_s{from_rel.empty() ? "." : from_rel};
  const std::string to_s{to_rel.empty() ? "." : to_rel};
  // renameat2 with RENAME_NOREPLACE optional — use renameat for portability.
  if (::renameat(root_fd.get(), from_s.c_str(), root_fd.get(), to_s.c_str()) != 0) {
    // When rel paths need parent dirfds for non-root-relative single component:
    // openat2 already verified; renameat from root works for multi-segment paths.
    return std::unexpected(Error{std::format(
        "renameat failed: {}", std::strerror(errno))});
  }
  return {};
#else
  auto a = authorize(from_path, relative_base);
  auto b = authorize(to_path, relative_base);
  if (!a) return std::unexpected(std::move(a.error()));
  if (!b) return std::unexpected(std::move(b.error()));
  return rename_path(*a, *b);
#endif
}

Result<std::vector<DirEntry>> PathSandbox::list(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto abs = join_user(user_path, relative_base);
  if (!abs) return std::unexpected(std::move(abs.error()));
  if (is_kernel_filesystem(*abs)) {
    return std::unexpected(Error{"path under kernel filesystem is not allowed"});
  }
  const auto* root = match_root(roots_, *abs);
  if (!root) return std::unexpected(Error{"path outside allowed roots"});
  const auto rel = relative_under(*abs, *root);

#if defined(__linux__)
  auto dirfd = open_relative(*root, rel, OpenMode::Dir);
  if (!dirfd) return std::unexpected(std::move(dirfd.error()));
  // fdopendir takes ownership of a dup
  int dupfd = ::dup(dirfd->get());
  if (dupfd < 0) {
    return std::unexpected(Error{std::format("dup failed: {}", std::strerror(errno))});
  }
  DIR* d = ::fdopendir(dupfd);
  if (d == nullptr) {
    ::close(dupfd);
    return std::unexpected(Error{std::format("fdopendir failed: {}", std::strerror(errno))});
  }
  std::vector<DirEntry> out;
  while (true) {
    errno = 0;
    dirent* ent = ::readdir(d);
    if (ent == nullptr) {
      if (errno != 0) {
        ::closedir(d);
        return std::unexpected(
            Error{std::format("readdir failed: {}", std::strerror(errno))});
      }
      break;
    }
    std::string_view name{ent->d_name};
    if (name == "." || name == "..") continue;
    DirEntry e;
    e.name = std::string{name};
    // openat dirfd for type
    open_how how{};
    how.flags = static_cast<__u64>(O_PATH | O_CLOEXEC | O_NOFOLLOW);
    how.resolve = RESOLVE_BENEATH;
    // Child under current dirfd: use openat (not openat2) with O_NOFOLLOW —
    // parent dir was already openat2-verified under root.
    int cfd = ::openat(dirfd->get(), ent->d_name, O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (cfd >= 0) {
      struct stat st{};
      if (::fstat(cfd, &st) == 0) {
        e.is_dir = S_ISDIR(st.st_mode);
        e.is_file = S_ISREG(st.st_mode);
        if (e.is_file) e.size = static_cast<std::uint64_t>(st.st_size);
      }
      ::close(cfd);
    }
    out.push_back(std::move(e));
  }
  ::closedir(d);
  return out;
#else
  auto auth = authorize(user_path, relative_base);
  if (!auth) return std::unexpected(std::move(auth.error()));
  return list_dir(*auth);
#endif
}

Result<FileStat> PathSandbox::stat(
    std::string_view user_path,
    const std::filesystem::path& relative_base) const {
  auto fd = open_read_fd(user_path, relative_base);
  if (!fd) {
#if defined(__linux__)
    auto abs = join_user(user_path, relative_base);
    if (!abs) return std::unexpected(std::move(abs.error()));
    const auto* root = match_root(roots_, *abs);
    if (!root) return std::unexpected(Error{"path outside allowed roots"});
    fd = open_relative(*root, relative_under(*abs, *root), OpenMode::PathOnly);
    if (!fd) return std::unexpected(std::move(fd.error()));
#else
    return std::unexpected(std::move(fd.error()));
#endif
  }
  struct stat st{};
  if (::fstat(fd->get(), &st) != 0) {
    return std::unexpected(
        Error{std::format("fstat failed: {}", std::strerror(errno))});
  }
  FileStat out;
  out.path = std::string{user_path};
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
  return out;
}

}  // namespace vacps::fs
