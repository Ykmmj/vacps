#pragma once

/**
 * Process environment captured once at startup.
 *
 * Policy (runtime layering Phase 1):
 * - Call EnvironmentSnapshot::from_current_process() (or BootstrapConfig::fromEnvironment())
 *   during process bootstrap only.
 * - host.getenv(name) must read from this snapshot, never std::getenv at request time.
 * - No other runtime code should call getenv after bootstrap (inject typed config or
 *   pass a reference to this snapshot instead).
 */

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vacps::bootstrap {

/**
 * Immutable-ish map of environment variables present when the process started.
 * Values may be empty strings when the var is set but empty (parity with POSIX getenv).
 */
class EnvironmentSnapshot {
 public:
  EnvironmentSnapshot() = default;

  /** Snapshot every entry from the process `environ` block. */
  [[nodiscard]] static EnvironmentSnapshot from_current_process();

  /** Build from an existing map (tests / injection). */
  explicit EnvironmentSnapshot(std::unordered_map<std::string, std::string> vars);

  /** @return value if name was present at capture; nullopt if unset. */
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const;

  /**
   * Like get(), but treats missing and empty-string as "not set"
   * (common for product config knobs).
   */
  [[nodiscard]] std::optional<std::string> get_nonempty(std::string_view name) const;

  [[nodiscard]] bool contains(std::string_view name) const;

  [[nodiscard]] const std::unordered_map<std::string, std::string>& entries() const noexcept {
    return vars_;
  }

  [[nodiscard]] std::size_t size() const noexcept { return vars_.size(); }

 private:
  std::unordered_map<std::string, std::string> vars_;
};

}  // namespace vacps::bootstrap
