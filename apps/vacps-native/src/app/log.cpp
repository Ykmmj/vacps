#include "app/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace vacps::log {
namespace {

spdlog::level::level_enum parse_level(std::string_view level) {
  if (level == "trace") return spdlog::level::trace;
  if (level == "debug") return spdlog::level::debug;
  if (level == "info") return spdlog::level::info;
  if (level == "warn" || level == "warning") return spdlog::level::warn;
  if (level == "error" || level == "err") return spdlog::level::err;
  if (level == "critical" || level == "fatal") return spdlog::level::critical;
  if (level == "off") return spdlog::level::off;
  return spdlog::level::info;
}

}  // namespace

void init(std::string_view level) {
  // stderr: keeps stdout free for any future machine-readable output.
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("vacps", std::move(sink));
  const auto lvl = parse_level(level);
  logger->set_level(lvl);
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(std::move(logger));
  spdlog::set_level(lvl);
}

void write_trace(std::string msg) { spdlog::trace("{}", msg); }
void write_debug(std::string msg) { spdlog::debug("{}", msg); }
void write_info(std::string msg) { spdlog::info("{}", msg); }
void write_warn(std::string msg) { spdlog::warn("{}", msg); }
void write_error(std::string msg) { spdlog::error("{}", msg); }
void write_critical(std::string msg) { spdlog::critical("{}", msg); }

void flush() {
  if (auto logger = spdlog::default_logger()) {
    logger->flush();
  }
}

}  // namespace vacps::log
