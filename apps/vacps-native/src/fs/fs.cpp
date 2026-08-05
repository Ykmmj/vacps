#include "fs/fs.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// renameat2 flags (uapi); fall back if headers omit them.
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#endif

namespace vacps::fs {
namespace {

bool contains_null(std::string_view s) {
  return s.find('\0') != std::string_view::npos;
}

bool is_not_found(const std::error_code& ec) {
  return ec == std::errc::no_such_file_or_directory ||
         ec == std::errc::not_a_directory;
}

}  // namespace

Error make_io_error(
    std::string_view operation,
    int errnum,
    std::string_view path) {
  const char* errstr = errnum != 0 ? std::strerror(errnum) : "unknown error";
  if (!path.empty()) {
    return Error{
        std::format(
            "{} failed ({}): {} (errno={})",
            operation,
            path,
            errstr,
            errnum),
        std::string{operation},
        errnum};
  }
  return Error{
      std::format("{} failed: {} (errno={})", operation, errstr, errnum),
      std::string{operation},
      errnum};
}

Error make_io_error(
    std::string_view operation,
    const std::error_code& ec,
    std::string_view path) {
  if (!path.empty()) {
    return Error{
        std::format(
            "{} failed ({}): {} (code={})",
            operation,
            path,
            ec.message(),
            ec.value()),
        std::string{operation},
        ec.value()};
  }
  return Error{
      std::format(
          "{} failed: {} (code={})", operation, ec.message(), ec.value()),
      std::string{operation},
      ec.value()};
}

Result<std::filesystem::path> resolve_path(
    const std::filesystem::path& workspace_root,
    std::string_view user_path) {
  if (user_path.empty()) {
    return std::unexpected(Error{"path is required", "resolve", 0});
  }
  if (contains_null(user_path)) {
    return std::unexpected(
        Error{"path contains a null byte", "resolve", 0});
  }

  const std::filesystem::path input{std::string{user_path}};
  if (input.is_absolute()) {
    return input.lexically_normal();
  }

  std::error_code ec;
  auto root = workspace_root;
  if (!root.is_absolute()) {
    root = std::filesystem::absolute(root, ec);
    if (ec) {
      return std::unexpected(make_io_error("resolve", ec, root.string()));
    }
  }
  return (root / input).lexically_normal();
}

VoidResult mkdir(const std::filesystem::path& path, MkdirOptions opts) {
  std::error_code ec;
  if (opts.recursive) {
    std::filesystem::create_directories(path, ec);
  } else {
    std::filesystem::create_directory(path, ec);
  }
  if (ec) {
    return std::unexpected(make_io_error("mkdir", ec, path.string()));
  }
  return {};
}

Result<bool> exists(const std::filesystem::path& path) {
  std::error_code ec;
  const bool present = std::filesystem::exists(path, ec);
  if (ec) {
    if (is_not_found(ec)) {
      return false;
    }
    return std::unexpected(make_io_error("exists", ec, path.string()));
  }
  return present;
}

VoidResult remove_path(
    const std::filesystem::path& path,
    RemoveOptions opts) {
  std::error_code ec;
  if (opts.recursive) {
    std::filesystem::remove_all(path, ec);
  } else {
    std::filesystem::remove(path, ec);
  }
  if (ec) {
    return std::unexpected(make_io_error("remove", ec, path.string()));
  }
  return {};
}

VoidResult rename_path(
    const std::filesystem::path& from,
    const std::filesystem::path& to,
    RenameOptions opts) {
#if defined(__linux__)
  // Atomic rename. replace=false uses renameat2(RENAME_NOREPLACE) — no TOCTOU.
  const unsigned int flags = opts.replace ? 0u : static_cast<unsigned int>(RENAME_NOREPLACE);
  if (::renameat2(
          AT_FDCWD,
          from.c_str(),
          AT_FDCWD,
          to.c_str(),
          flags) == 0) {
    return {};
  }
  const int err = errno;
  if (err == EINVAL && !opts.replace) {
    // Kernel/fs may not support RENAME_NOREPLACE — do not silently race.
    return std::unexpected(Error{
        std::format(
            "rename replace=false unsupported on this filesystem ({} → {}): "
            "renameat2 RENAME_NOREPLACE not available (errno={})",
            from.string(),
            to.string(),
            err),
        "rename",
        err});
  }
  if (err == ENOSYS && !opts.replace) {
    return std::unexpected(Error{
        std::format(
            "rename replace=false unsupported: renameat2 not available "
            "(errno={})",
            err),
        "rename",
        err});
  }
  return std::unexpected(make_io_error("rename", err, from.string()));
#else
  (void)opts;
  return std::unexpected(
      Error{"rename requires Linux renameat2", "rename", ENOSYS});
#endif
}

Result<std::vector<DirEntry>> list_dir(const std::filesystem::path& path) {
  std::error_code ec;
  const bool is_dir = std::filesystem::is_directory(path, ec);
  if (ec) {
    return std::unexpected(make_io_error("list", ec, path.string()));
  }
  if (!is_dir) {
    return std::unexpected(Error{
        std::format("not a directory: {}", path.string()), "list", ENOTDIR});
  }

  std::vector<DirEntry> out;
  // Do not skip_permission_denied — surface permission errors.
  std::filesystem::directory_iterator it(path, ec);
  if (ec) {
    return std::unexpected(make_io_error("list", ec, path.string()));
  }
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return std::unexpected(make_io_error("list", ec, path.string()));
    }
    DirEntry e;
    e.name = it->path().filename().string();

    // lstat classification via symlink_status — dangling symlink is an entry,
    // not a list failure, and is not followed as file/dir.
    std::error_code st_ec;
    const auto st = it->symlink_status(st_ec);
    if (st_ec) {
      return std::unexpected(
          make_io_error("list", st_ec, it->path().string()));
    }
    e.is_symlink = std::filesystem::is_symlink(st);
    e.is_dir = std::filesystem::is_directory(st);
    e.is_file = std::filesystem::is_regular_file(st);
    if (e.is_file) {
      st_ec.clear();
      // file_size follows the entry; for regular files only (not symlinks).
      e.size = static_cast<std::uint64_t>(it->file_size(st_ec));
      if (st_ec) {
        return std::unexpected(
            make_io_error("list", st_ec, it->path().string()));
      }
    } else if (e.is_symlink) {
#if defined(__linux__)
      struct stat lst{};
      if (::lstat(it->path().c_str(), &lst) == 0) {
        e.size = static_cast<std::uint64_t>(lst.st_size);
      }
#endif
    }
    out.push_back(std::move(e));
  }
  return out;
}

Result<FileStat> file_stat(const std::filesystem::path& path) {
  // Path stat: lstat semantics — dangling symlink reports type symlink.
#if defined(__linux__)
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    return std::unexpected(make_io_error("stat", errno, path.string()));
  }
  FileStat out;
  out.path = path.string();
  out.is_symlink = S_ISLNK(st.st_mode);
  if (out.is_symlink) {
    out.type = "symlink";
    // size of the symlink contents (path string length)
    out.size_bytes = static_cast<std::uint64_t>(st.st_size);
  } else if (S_ISDIR(st.st_mode)) {
    out.type = "directory";
  } else if (S_ISREG(st.st_mode)) {
    out.type = "file";
    out.size_bytes = static_cast<std::uint64_t>(st.st_size);
  } else {
    out.type = "other";
  }
  out.modified_at_ms = static_cast<std::int64_t>(st.st_mtime) * 1000;
  // access(2) follows symlinks; for symlink entries report link node mode bits.
  if (out.is_symlink) {
    out.readable = (st.st_mode & S_IRUSR) != 0;
    out.writable = (st.st_mode & S_IWUSR) != 0;
  } else {
    out.readable = (::access(path.c_str(), R_OK) == 0);
    out.writable = (::access(path.c_str(), W_OK) == 0);
  }
  return out;
#else
  return std::unexpected(Error{"stat requires Linux", "stat", ENOSYS});
#endif
}

}  // namespace vacps::fs
