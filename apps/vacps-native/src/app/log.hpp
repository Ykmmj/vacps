#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::log {

/** Initialize the default stderr logger. Call once before other log APIs.
 *  @param level  trace|debug|info|warn|error|critical|off (default: info)
 */
void init(std::string_view level = "info");

void flush();

// Implemented in log.cpp. Prefer the formatted overloads below.
void write_trace(std::string msg);
void write_debug(std::string msg);
void write_info(std::string msg);
void write_warn(std::string msg);
void write_error(std::string msg);
void write_critical(std::string msg);

// Always use format-style API (C++23). For a raw runtime string: log::info("{}", s).
template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
  write_trace(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  write_debug(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  write_info(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  write_warn(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  write_error(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void critical(std::format_string<Args...> fmt, Args&&... args) {
  write_critical(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace vacps::log
