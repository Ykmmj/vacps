#include "bootstrap/environment.hpp"

#include <cstdlib>
#include <utility>

// POSIX process environment block (musl / glibc).
extern char** environ;

namespace vacps::bootstrap {

EnvironmentSnapshot::EnvironmentSnapshot(std::unordered_map<std::string, std::string> vars)
    : vars_(std::move(vars)) {}

EnvironmentSnapshot EnvironmentSnapshot::from_current_process() {
  std::unordered_map<std::string, std::string> vars;
  if (environ == nullptr) {
    return EnvironmentSnapshot{std::move(vars)};
  }
  for (char** p = environ; *p != nullptr; ++p) {
    const char* entry = *p;
    if (entry == nullptr || entry[0] == '\0') {
      continue;
    }
    const char* eq = entry;
    while (*eq != '\0' && *eq != '=') {
      ++eq;
    }
    if (*eq != '=') {
      // Malformed entry without '='; skip.
      continue;
    }
    std::string key{entry, static_cast<std::size_t>(eq - entry)};
    if (key.empty()) {
      continue;
    }
    vars.emplace(std::move(key), std::string{eq + 1});
  }
  return EnvironmentSnapshot{std::move(vars)};
}

std::optional<std::string> EnvironmentSnapshot::get(std::string_view name) const {
  if (name.empty()) {
    return std::nullopt;
  }
  const auto it = vars_.find(std::string{name});
  if (it == vars_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> EnvironmentSnapshot::get_nonempty(std::string_view name) const {
  auto v = get(name);
  if (!v || v->empty()) {
    return std::nullopt;
  }
  return v;
}

bool EnvironmentSnapshot::contains(std::string_view name) const {
  if (name.empty()) {
    return false;
  }
  return vars_.contains(std::string{name});
}

}  // namespace vacps::bootstrap
