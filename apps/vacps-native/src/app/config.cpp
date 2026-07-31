#include "app/config.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace vacps {
namespace {

std::string env_or(const char* key, std::string fallback) {
  if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0') {
    return v;
  }
  return fallback;
}

std::uint16_t env_port(const char* key, std::uint16_t fallback) {
  const char* v = std::getenv(key);
  if (v == nullptr || v[0] == '\0') return fallback;
  auto p = parse_port(v);
  return p ? *p : fallback;
}

std::vector<std::string> parse_fs_allowed_roots() {
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
    // trim
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

}  // namespace

bool is_loopback_host(std::string_view host) noexcept {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool remote_bind_allowed_from_env() noexcept {
  return env_or("VACPS_ALLOW_REMOTE_BIND", "false") == "true";
}

void apply_remote_bind_policy(Config& cfg) noexcept {
  if (!remote_bind_allowed_from_env() && !is_loopback_host(cfg.listen_host)) {
    cfg.listen_host = "127.0.0.1";
  }
}

Result<std::uint16_t> parse_port(std::string_view text) {
  if (text.empty()) {
    return std::unexpected(Error{"port is empty"});
  }
  unsigned long n = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, n);
  if (ec != std::errc{} || ptr != end || n == 0 || n > 65535) {
    return std::unexpected(Error{"port must be an integer in 1..65535"});
  }
  return static_cast<std::uint16_t>(n);
}

Config Config::from_env() {
  Config c;
  c.listen_host = env_or("VACPS_LISTEN_HOST", c.listen_host);
  c.listen_port = env_port("VACPS_LISTEN_PORT", c.listen_port);
  c.log_level = env_or("VACPS_LOG_LEVEL", c.log_level);
  c.data_dir = env_or("VACPS_DATA_DIR", c.data_dir);
  c.ca_bundle = env_or("VACPS_CA_BUNDLE", c.ca_bundle);
  c.fs_allowed_roots = parse_fs_allowed_roots();
  apply_remote_bind_policy(c);
  return c;
}

}  // namespace vacps
