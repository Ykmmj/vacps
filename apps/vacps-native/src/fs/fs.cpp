#include "fs/fs.hpp"

#include <chrono>
#include <fstream>
#include <format>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace vacps::fs {
namespace {

VoidResult io_error(std::string_view op, const std::filesystem::path& p, const std::error_code& ec) {
  return std::unexpected(Error{std::format("{} failed ({}): {}", op, p.string(), ec.message())});
}

bool contains_null(std::string_view s) {
  return s.find('\0') != std::string_view::npos;
}

}  // namespace

Result<std::filesystem::path> resolve_path(
    const std::filesystem::path& workspace_root,
    std::string_view user_path) {
  if (user_path.empty()) {
    return std::unexpected(Error{"path is required"});
  }
  // Embedded NUL cannot be expressed to open(2); technical limit, not product policy.
  if (contains_null(user_path)) {
    return std::unexpected(Error{"path contains a null byte"});
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
      return std::unexpected(Error{std::format("invalid workspace root: {}", ec.message())});
    }
  }
  return (root / input).lexically_normal();
}

Result<std::string> read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error{std::format("read failed: {}", path.string())});
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Result<std::vector<std::uint8_t>> read_bytes(const std::filesystem::path& path) {
  auto text = read_text(path);
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  return std::vector<std::uint8_t>(text->begin(), text->end());
}

VoidResult write_text(const std::filesystem::path& path, std::string_view data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return io_error("mkdir", path.parent_path(), ec);
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return std::unexpected(Error{std::format("write failed: {}", path.string())});
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out) {
    return std::unexpected(Error{std::format("write failed: {}", path.string())});
  }
  return {};
}

VoidResult write_bytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data) {
  return write_text(
      path,
      std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

VoidResult append_text(const std::filesystem::path& path, std::string_view data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return io_error("mkdir", path.parent_path(), ec);
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    return std::unexpected(Error{std::format("append failed: {}", path.string())});
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out) {
    return std::unexpected(Error{std::format("append failed: {}", path.string())});
  }
  return {};
}

VoidResult mkdir_p(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return io_error("mkdir", path, ec);
  }
  return {};
}

bool exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

VoidResult remove_path(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  if (ec) {
    return io_error("remove", path, ec);
  }
  return {};
}

VoidResult rename_path(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  if (to.has_parent_path()) {
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) {
      return io_error("mkdir", to.parent_path(), ec);
    }
  }
  std::filesystem::rename(from, to, ec);
  if (ec) {
    return io_error("rename", from, ec);
  }
  return {};
}

Result<std::vector<DirEntry>> list_dir(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    return std::unexpected(Error{std::format("not a directory: {}", path.string())});
  }
  std::vector<DirEntry> out;
  for (const auto& ent : std::filesystem::directory_iterator(path, ec)) {
    if (ec) {
      return std::unexpected(Error{std::format("list failed: {}", ec.message())});
    }
    DirEntry e;
    e.name = ent.path().filename().string();
    e.is_dir = ent.is_directory(ec);
    e.is_file = ent.is_regular_file(ec);
    if (e.is_file) {
      e.size = static_cast<std::uint64_t>(ent.file_size(ec));
    }
    out.push_back(std::move(e));
  }
  return out;
}

Result<FileStat> file_stat(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::unexpected(Error{std::format("path not found: {}", path.string())});
  }
  FileStat st;
  st.path = path.string();
  st.is_symlink = std::filesystem::is_symlink(path, ec);
  if (std::filesystem::is_directory(path, ec)) {
    st.type = "directory";
  } else if (st.is_symlink) {
    st.type = "symlink";
  } else if (std::filesystem::is_regular_file(path, ec)) {
    st.type = "file";
    st.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    if (ec) st.size_bytes = 0;
  } else {
    st.type = "other";
  }
  auto ftime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());
    st.modified_at_ms =
        duration_cast<milliseconds>(sctp.time_since_epoch()).count();
  }
#if defined(__linux__)
  st.readable = (::access(path.c_str(), R_OK) == 0);
  st.writable = (::access(path.c_str(), W_OK) == 0);
#else
  st.readable = true;
  st.writable = true;
#endif
  return st;
}

}  // namespace vacps::fs
