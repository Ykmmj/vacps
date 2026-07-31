#pragma once

#include "app/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vacps {

struct Config {
  std::string listen_host{"127.0.0.1"};
  std::uint16_t listen_port{8788};
  std::string log_level{"info"};
  /** Directory for agent.db and other local state (env VACPS_DATA_DIR). */
  std::string data_dir{"data"};
  /**
   * PEM CA bundle for outbound HTTPS (env VACPS_CA_BUNDLE).
   * Empty → resolve platform defaults at request time (fail-closed if missing).
   */
  std::string ca_bundle;
  /**
   * Extra absolute filesystem roots for vacps:fs PathSandbox
   * (env VACPS_FS_ALLOWED_ROOTS, colon/comma separated).
   * Always includes data_dir and /tmp.
   */
  std::vector<std::string> fs_allowed_roots;

  /** Load from environment (command line overrides applied separately). */
  static Config from_env();

  [[nodiscard]] std::string database_path() const {
    if (data_dir.empty() || data_dir.back() == '/') return data_dir + "agent.db";
    return data_dir + "/agent.db";
  }
};

/** True for 127.0.0.1 / localhost / ::1. */
[[nodiscard]] bool is_loopback_host(std::string_view host) noexcept;

/** VACPS_ALLOW_REMOTE_BIND=true. */
[[nodiscard]] bool remote_bind_allowed_from_env() noexcept;

/**
 * Fail-closed: non-loopback listen_host is forced to 127.0.0.1 unless
 * VACPS_ALLOW_REMOTE_BIND=true. Call after CLI overrides.
 */
void apply_remote_bind_policy(Config& cfg) noexcept;

/** Parse port in 1..65535; rejects overflow and empty. */
[[nodiscard]] Result<std::uint16_t> parse_port(std::string_view text);

}  // namespace vacps
