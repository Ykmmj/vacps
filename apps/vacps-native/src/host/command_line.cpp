#include "host/command_line.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace vacps::host {
namespace {

[[nodiscard]] CommandLineError make_error(
    std::string message,
    std::string option = {}) {
  return CommandLineError{std::move(message), std::move(option)};
}

[[nodiscard]] CommandLineError unknown_option(std::string_view arg) {
  return make_error(std::format("unknown option: {}", arg), std::string{arg});
}

[[nodiscard]] CommandLineError missing_value(std::string_view option) {
  return make_error(
      std::format("missing value for option {}", option),
      std::string{option});
}

[[nodiscard]] CommandLineError empty_value(std::string_view option) {
  return make_error(
      std::format("empty value for option {}", option),
      std::string{option});
}

[[nodiscard]] CommandLineError duplicate_option(std::string_view option) {
  return make_error(
      std::format("duplicate option {}", option),
      std::string{option});
}

[[nodiscard]] CommandLineError invalid_value(
    std::string_view option,
    std::string_view raw,
    std::string_view reason) {
  return make_error(
      std::format(
          "invalid value for option {}: '{}' ({})",
          option,
          raw,
          reason),
      std::string{option});
}

/** Full-string unsigned parse; rejects empty, sign, suffix, overflow. */
template <class T>
[[nodiscard]] std::expected<T, CommandLineError> parse_unsigned_full(
    std::string_view option,
    std::string_view raw) {
  if (raw.empty()) {
    return std::unexpected(empty_value(option));
  }
  T value{};
  const char* const first = raw.data();
  const char* const last = raw.data() + raw.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec == std::errc::result_out_of_range) {
    return std::unexpected(invalid_value(option, raw, "out of range"));
  }
  if (ec != std::errc{} || ptr != last) {
    return std::unexpected(invalid_value(option, raw, "invalid syntax"));
  }
  return value;
}

[[nodiscard]] std::expected<std::chrono::milliseconds, CommandLineError>
parse_milliseconds(std::string_view option, std::string_view raw) {
  using Rep = std::chrono::milliseconds::rep;
  const auto parsed = parse_unsigned_full<unsigned long long>(option, raw);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  constexpr auto k_max = static_cast<unsigned long long>(
      (std::numeric_limits<Rep>::max)());
  if (*parsed > k_max) {
    return std::unexpected(invalid_value(
        option,
        raw,
        "exceeds std::chrono::milliseconds range"));
  }
  return std::chrono::milliseconds{static_cast<Rep>(*parsed)};
}

[[nodiscard]] bool is_canonical_log_level(std::string_view level) noexcept {
  return level == "trace" || level == "debug" || level == "info" ||
         level == "warn" || level == "error" || level == "critical" ||
         level == "off";
}

struct SeenFlags {
  bool script{false};
  bool data_dir{false};
  bool log_level{false};
  bool ca_bundle{false};
  bool js_heap{false};
  bool js_stack{false};
  bool js_time{false};
  bool lifecycle{false};
  bool help{false};
  bool version{false};
};

[[nodiscard]] std::expected<std::string_view, CommandLineError> require_value(
    std::string_view option,
    std::span<const std::string_view> argv,
    std::size_t* index) {
  if (*index + 1 >= argv.size()) {
    return std::unexpected(missing_value(option));
  }
  ++(*index);
  const std::string_view value = argv[*index];
  // Values must not look like flags; no leading '-' and no '--' escaping.
  if (value.empty()) {
    return std::unexpected(empty_value(option));
  }
  if (value.starts_with('-')) {
    return std::unexpected(missing_value(option));
  }
  return value;
}

}  // namespace

std::string format_usage(std::string_view program_name) {
  const std::string_view name =
      program_name.empty() ? std::string_view{"vacps-agent"} : program_name;
  return std::format(
      "Usage: {} [options]\n"
      "\n"
      "Actions (must be used alone):\n"
      "  -h, --help                 Show this help and exit\n"
      "  -V, --version              Print version and exit\n"
      "\n"
      "Options:\n"
      "  --script PATH              Business ESM bundle\n"
      "  --data-dir DIR             Data directory (default: data)\n"
      "  --log-level LEVEL          Log level: trace|debug|info|warn|error|critical|off\n"
      "                             (default: info)\n"
      "  --ca-bundle PATH           CA bundle for outbound TLS (vacps:http)\n"
      "  --js-heap-limit-bytes N    QuickJS heap limit in bytes (N > 0)\n"
      "  --js-stack-limit-bytes N   QuickJS stack limit in bytes (N > 0)\n"
      "  --js-time-budget-ms N      JS CPU time budget in ms (N >= 0; 0 disables)\n"
      "  --lifecycle-timeout-ms N   Entry initialize/shutdown timeout ms (N > 0;\n"
      "                             default: 30000)\n"
      "\n"
      "C++ process knobs are CLI-only (no VACPS_* environment fallbacks).\n"
      "Product policy (listen, auth, control-plane keys, FS roots, …) is owned\n"
      "by the business script via host.getenv().\n"
      "Syntax is space-separated (--flag value); --flag=value is not accepted.\n",
      name);
}

std::expected<ParsedCommandLine, CommandLineError> parse_command_line(
    std::span<const std::string_view> argv) {
  if (argv.empty()) {
    return std::unexpected(make_error("empty argv"));
  }

  ParsedCommandLine parsed{};
  SeenFlags seen{};

  for (std::size_t i = 1; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];

    if (arg.empty()) {
      return std::unexpected(make_error("empty argument"));
    }

    // Reject --flag=value form deliberately (not implemented).
    if (arg.starts_with("-") && arg.contains('=')) {
      return std::unexpected(unknown_option(arg));
    }

    if (arg == "--help" || arg == "-h") {
      if (seen.help) {
        return std::unexpected(duplicate_option(arg));
      }
      seen.help = true;
      continue;
    }
    if (arg == "--version" || arg == "-V") {
      if (seen.version) {
        return std::unexpected(duplicate_option(arg));
      }
      seen.version = true;
      continue;
    }

    if (arg == "--script") {
      if (seen.script) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      parsed.options.script_path = std::string{*value};
      seen.script = true;
      continue;
    }
    if (arg == "--data-dir") {
      if (seen.data_dir) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      parsed.options.data_dir = std::string{*value};
      seen.data_dir = true;
      continue;
    }
    if (arg == "--log-level") {
      if (seen.log_level) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (!is_canonical_log_level(*value)) {
        return std::unexpected(invalid_value(
            arg,
            *value,
            "expected trace|debug|info|warn|error|critical|off"));
      }
      parsed.options.log_level = std::string{*value};
      seen.log_level = true;
      continue;
    }
    if (arg == "--ca-bundle") {
      if (seen.ca_bundle) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      parsed.options.ca_bundle = std::string{*value};
      seen.ca_bundle = true;
      continue;
    }
    if (arg == "--js-heap-limit-bytes") {
      if (seen.js_heap) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      auto n = parse_unsigned_full<std::size_t>(arg, *value);
      if (!n) {
        return std::unexpected(n.error());
      }
      if (*n == 0) {
        return std::unexpected(invalid_value(
            arg, *value, "must be greater than zero"));
      }
      parsed.options.runtime.engine.heap_limit_bytes = *n;
      seen.js_heap = true;
      continue;
    }
    if (arg == "--js-stack-limit-bytes") {
      if (seen.js_stack) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      auto n = parse_unsigned_full<std::size_t>(arg, *value);
      if (!n) {
        return std::unexpected(n.error());
      }
      if (*n == 0) {
        return std::unexpected(invalid_value(
            arg, *value, "must be greater than zero"));
      }
      parsed.options.runtime.engine.stack_limit_bytes = *n;
      seen.js_stack = true;
      continue;
    }
    if (arg == "--js-time-budget-ms") {
      if (seen.js_time) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      // N >= 0; zero disables the interrupt watchdog.
      auto n = parse_milliseconds(arg, *value);
      if (!n) {
        return std::unexpected(n.error());
      }
      parsed.options.runtime.engine.js_time_budget = *n;
      seen.js_time = true;
      continue;
    }
    if (arg == "--lifecycle-timeout-ms") {
      if (seen.lifecycle) {
        return std::unexpected(duplicate_option(arg));
      }
      auto value = require_value(arg, argv, &i);
      if (!value) {
        return std::unexpected(value.error());
      }
      auto n = parse_milliseconds(arg, *value);
      if (!n) {
        return std::unexpected(n.error());
      }
      if (n->count() == 0) {
        return std::unexpected(invalid_value(
            arg, *value, "must be greater than zero"));
      }
      parsed.options.lifecycle_timeout = *n;
      seen.lifecycle = true;
      continue;
    }

    if (arg.starts_with("-")) {
      return std::unexpected(unknown_option(arg));
    }
    return std::unexpected(make_error(
        std::format("unexpected positional argument: {}", arg)));
  }

  const bool any_value_option = seen.script || seen.data_dir ||
                                seen.log_level || seen.ca_bundle ||
                                seen.js_heap || seen.js_stack ||
                                seen.js_time || seen.lifecycle;

  if (seen.help && seen.version) {
    return std::unexpected(make_error(
        "options --help and --version cannot be combined",
        "--help"));
  }
  if (seen.help) {
    if (any_value_option) {
      return std::unexpected(make_error(
          "--help must be used alone",
          "--help"));
    }
    parsed.action = CommandLineAction::help;
    return parsed;
  }
  if (seen.version) {
    if (any_value_option) {
      return std::unexpected(make_error(
          "--version must be used alone",
          "--version"));
    }
    parsed.action = CommandLineAction::version;
    return parsed;
  }

  parsed.action = CommandLineAction::run;
  return parsed;
}

}  // namespace vacps::host
