#include "bootstrap/config.hpp"

#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

namespace vacps::bootstrap {
namespace {

[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view raw) {
  if (raw.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  const char* first = raw.data();
  const char* last = raw.data() + raw.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

BootstrapConfig BootstrapConfig::fromEnvironment() {
  return from_snapshot(EnvironmentSnapshot::from_current_process());
}

BootstrapConfig BootstrapConfig::from_snapshot(EnvironmentSnapshot snapshot) {
  BootstrapConfig cfg;
  cfg.environment = std::move(snapshot);
  const EnvironmentSnapshot& env = cfg.environment;

  // Process bootstrap only. Product policy (LISTEN_*, ALLOW_INSECURE_*,
  // CONTROL_PLANE_*, FS product roots, …) is NOT typed here — JS reads them
  // via host.getenv() / EnvironmentSnapshot.

  if (auto v = env.get_nonempty("VACPS_DATA_DIR")) {
    cfg.data_dir = std::move(*v);
  }
  if (auto v = env.get_nonempty("VACPS_LOG_LEVEL")) {
    cfg.log_level = std::move(*v);
  }
  if (auto v = env.get_nonempty("VACPS_SCRIPT")) {
    cfg.script_path = std::move(*v);
  }
  if (auto v = env.get_nonempty("VACPS_CA_BUNDLE")) {
    cfg.ca_bundle = std::move(*v);
  }

  if (auto raw = env.get_nonempty("VACPS_JS_HEAP_LIMIT_BYTES")) {
    if (auto n = parse_size(*raw); n && *n > 0) {
      cfg.js_heap_limit_bytes = *n;
    }
  }
  if (auto raw = env.get_nonempty("VACPS_JS_STACK_LIMIT_BYTES")) {
    if (auto n = parse_size(*raw); n && *n > 0) {
      cfg.js_stack_limit_bytes = *n;
    }
  }
  if (auto raw = env.get_nonempty("VACPS_JS_TIME_BUDGET_MS")) {
    // 0 is valid (disable watchdog).
    if (auto n = parse_size(*raw)) {
      cfg.js_time_budget =
          std::chrono::milliseconds{static_cast<std::int64_t>(*n)};
    }
  }

  return cfg;
}

}  // namespace vacps::bootstrap
