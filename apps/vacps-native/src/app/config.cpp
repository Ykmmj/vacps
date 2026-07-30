#include "app/config.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>

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
  apply_remote_bind_policy(c);
  return c;
}

}  // namespace vacps
