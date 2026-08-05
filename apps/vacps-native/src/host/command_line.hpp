#pragma once

/**
 * Strict argv parser for vacps-agent.
 *
 * Independently testable: accepts std::span<const std::string_view> (full argv,
 * including argv[0]). Does not read the environment. Application does not parse
 * argv — main adapts argc/argv, then either handles help/version or builds
 * Application from ParsedCommandLine::options.
 *
 * Syntax is space-separated only (`--flag value`). `--flag=value` is rejected
 * as an unknown option. Value options may appear at most once. Help/version are
 * actions and must be used alone (apart from argv[0]); combining them is an error.
 *
 * C++ process knobs are CLI-only. Product policy remains JS env via host.getenv().
 */

#include "host/application.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace vacps::host {

struct CommandLineError {
  std::string message;
  /** Offending flag/option name when applicable; empty otherwise. */
  std::string option;
};

enum class CommandLineAction {
  run,
  help,
  version,
};

struct ParsedCommandLine {
  CommandLineAction action{CommandLineAction::run};
  Application::Options options{};
};

/**
 * Parse full argv (argv[0] = program name). Empty argv is an error.
 * On success, action is help/version/run; options hold defaults plus any
 * value-option overrides. Help/version leave options at defaults.
 */
[[nodiscard]] std::expected<ParsedCommandLine, CommandLineError>
parse_command_line(std::span<const std::string_view> argv);

/** Usage text including the option table (program name substituted). */
[[nodiscard]] std::string format_usage(std::string_view program_name);

}  // namespace vacps::host
