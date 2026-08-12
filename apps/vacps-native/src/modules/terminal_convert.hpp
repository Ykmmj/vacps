#pragma once

#include "binding/convert.hpp"
#include "qjs/owned_value.hpp"
#include "terminal/terminal.hpp"

#include <quickjs.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::js::terminal_module {

struct OptionalStringArgs {
  std::vector<std::string> args;
};

struct OptionsDecode {
  terminal::StartOptions options;
};

struct WritePayload {
  std::vector<std::uint8_t> data;
};

struct ReadOptionsDecode {
  terminal::ReadOptions options;
};

struct Columns {
  std::uint16_t value{80};
};

struct Rows {
  std::uint16_t value{24};
};

struct Signal {
  int value{0};
};

struct WaitTimeout {
  std::optional<std::chrono::milliseconds> value;
};

struct CloseGrace {
  std::chrono::milliseconds value{1000};
};

}  // namespace vacps::js::terminal_module

namespace vacps::binding {

namespace tm = vacps::js::terminal_module;

namespace terminal_detail {

[[nodiscard]] inline bool nullish(JSValueConst value) noexcept {
  return JS_IsUndefined(value) || JS_IsNull(value);
}

[[nodiscard]] inline Result<qjs::OwnedValue> get(
    Env env,
    JSValueConst object,
    const char* name) {
  qjs::OwnedValue value =
      qjs::OwnedValue::get_property_str(env.context(), object, name);
  if (value.is_exception()) {
    clear_exception(env.context());
    (void)value.release();
    return std::unexpected(
        Error::type(std::string{"failed to read property '"} + name + "'"));
  }
  return value;
}

[[nodiscard]] inline Result<void> require_object(
    Env env,
    JSValueConst value,
    const char* label) {
  if (!JS_IsObject(value)) {
    return std::unexpected(Error::type(std::string{label} + " must be an object"));
  }
  const int array = JS_IsArray(env.context(), value);
  if (array < 0) {
    clear_exception(env.context());
    return std::unexpected(Error::type(std::string{label} + " must be an object"));
  }
  if (array != 0) {
    return std::unexpected(Error::type(std::string{label} + " must be an object"));
  }
  return {};
}

template <class T>
[[nodiscard]] inline Result<T> bounded(
    Env env,
    JSValueConst value,
    const char* label,
    std::uint64_t minimum,
    std::uint64_t maximum) {
  auto number = Converter<std::uint64_t>::from_js(env, value);
  if (!number) {
    return std::unexpected(Error::type(std::string{label} + " must be an integer"));
  }
  if (*number < minimum || *number > maximum) {
    return std::unexpected(Error::range(std::format(
        "{} must be an integer in [{}, {}]", label, minimum, maximum)));
  }
  return static_cast<T>(*number);
}

[[nodiscard]] inline Result<std::vector<std::string>> string_array(
    Env env,
    JSValueConst value) {
  if (nullish(value)) return std::vector<std::string>{};
  const int array = JS_IsArray(env.context(), value);
  if (array <= 0) {
    if (array < 0) clear_exception(env.context());
    return std::unexpected(Error::type("args must be an array of strings"));
  }
  auto length_value = get(env, value, "length");
  if (!length_value) return std::unexpected(std::move(length_value.error()));
  auto length = bounded<std::uint32_t>(
      env, length_value->get(), "args.length", 0, 1000);
  if (!length) return std::unexpected(std::move(length.error()));
  std::vector<std::string> result;
  result.reserve(*length);
  for (std::uint32_t index = 0; index < *length; ++index) {
    qjs::OwnedValue element{
        env.context(), JS_GetPropertyUint32(env.context(), value, index)};
    if (element.is_exception()) {
      clear_exception(env.context());
      (void)element.release();
      return std::unexpected(Error::type("args must be an array of strings"));
    }
    auto string = Converter<std::string>::from_js(env, element.get());
    if (!string) {
      return std::unexpected(
          Error::type(std::format("args[{}] must be a string", index)));
    }
    result.push_back(std::move(*string));
  }
  return result;
}

[[nodiscard]] inline bool valid_environment_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 128) return false;
  const auto first = static_cast<unsigned char>(name.front());
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
        first == '_')) {
    return false;
  }
  for (const unsigned char character : name.substr(1)) {
    if (!((character >= 'A' && character <= 'Z') ||
          (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_')) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline Result<std::vector<terminal::EnvironmentVariable>>
environment_from_js(Env env, JSValueConst value) {
  if (nullish(value)) return std::vector<terminal::EnvironmentVariable>{};
  if (auto object = require_object(env, value, "TerminalOptions.environment"); !object) {
    return std::unexpected(std::move(object.error()));
  }

  JSContext* ctx = env.context();
  JSPropertyEnum* properties = nullptr;
  std::uint32_t length = 0;
  if (JS_GetOwnPropertyNames(
          ctx,
          &properties,
          &length,
          value,
          JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type("failed to enumerate TerminalOptions.environment"));
  }
  struct PropertyGuard {
    JSContext* context;
    JSPropertyEnum* properties;
    std::uint32_t length;
    ~PropertyGuard() { JS_FreePropertyEnum(context, properties, length); }
  } guard{ctx, properties, length};

  if (length > 256) {
    return std::unexpected(
        Error::range("TerminalOptions.environment may have at most 256 entries"));
  }
  std::vector<terminal::EnvironmentVariable> result;
  result.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    qjs::OwnedValue key{ctx, JS_AtomToValue(ctx, properties[index].atom)};
    if (key.is_exception()) {
      clear_exception(ctx);
      (void)key.release();
      return std::unexpected(
          Error::type("failed to read TerminalOptions.environment key"));
    }
    auto name = Converter<std::string>::from_js(env, key.get());
    if (!name || !valid_environment_name(*name)) {
      return std::unexpected(Error::type(
          "TerminalOptions.environment contains an invalid variable name"));
    }

    qjs::OwnedValue property{
        ctx, JS_GetProperty(ctx, value, properties[index].atom)};
    if (property.is_exception()) {
      clear_exception(ctx);
      (void)property.release();
      return std::unexpected(Error::type(
          std::string{"failed to read TerminalOptions.environment['"} + *name + "']"));
    }
    if (!JS_IsString(property.get())) {
      return std::unexpected(Error::type(
          std::string{"TerminalOptions.environment['"} + *name + "'] must be a string"));
    }
    auto decoded = Converter<std::string>::from_js(env, property.get());
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    if (decoded->size() > 65'536 || decoded->find('\0') != std::string::npos) {
      return std::unexpected(Error::range(
          std::string{"TerminalOptions.environment['"} + *name +
          "'] must be at most 65536 bytes and contain no null byte"));
    }
    result.push_back(terminal::EnvironmentVariable{
        .name = std::move(*name),
        .value = std::move(*decoded),
    });
  }
  return result;
}

[[nodiscard]] inline std::string_view status_name(
    terminal::TerminalStatus status) noexcept {
  switch (status) {
    case terminal::TerminalStatus::Created:
    case terminal::TerminalStatus::Starting:
    case terminal::TerminalStatus::Running:
      return "running";
    case terminal::TerminalStatus::Exited:
      return "exited";
    case terminal::TerminalStatus::Signaled:
      return "signaled";
    case terminal::TerminalStatus::TimedOut:
      return "timed_out";
  }
  std::unreachable();
}

[[nodiscard]] inline std::string signal_name(int signal) {
  switch (signal) {
    case SIGINT:
      return "SIGINT";
    case SIGTERM:
      return "SIGTERM";
    case SIGHUP:
      return "SIGHUP";
    case SIGKILL:
      return "SIGKILL";
    case SIGTSTP:
      return "SIGTSTP";
    case SIGCONT:
      return "SIGCONT";
    default:
      return std::format("SIG{}", signal);
  }
}

[[nodiscard]] inline bool set(
    Env env,
    JSValueConst object,
    const char* key,
    qjs::OwnedValue value) {
  if (value.is_exception()) return false;
  return JS_SetPropertyStr(env.context(), object, key, value.release()) >= 0;
}

[[nodiscard]] inline bool set_exit(
    Env env,
    JSValueConst object,
    const terminal::ExitResult& exit) {
  return set(env, object, "status", env.string(status_name(exit.status))) &&
         set(
             env,
             object,
             "exitCode",
             exit.exit_code
                 ? Converter<std::int32_t>::to_js(env, *exit.exit_code)
                 : env.null_value()) &&
         set(
             env,
             object,
             "signal",
             exit.signal ? env.string(signal_name(*exit.signal)) : env.null_value()) &&
         set(env, object, "timedOut", Converter<bool>::to_js(env, exit.timed_out));
}

}  // namespace terminal_detail

template <>
struct Converter<tm::OptionalStringArgs> {
  static Result<tm::OptionalStringArgs> from_js(Env env, JSValueConst value) {
    auto args = terminal_detail::string_array(env, value);
    if (!args) return std::unexpected(std::move(args.error()));
    return tm::OptionalStringArgs{.args = std::move(*args)};
  }
};

template <>
struct Converter<tm::OptionsDecode> {
  static Result<tm::OptionsDecode> from_js(Env env, JSValueConst value) {
    tm::OptionsDecode result;
    if (terminal_detail::nullish(value)) return result;
    if (auto object = terminal_detail::require_object(env, value, "TerminalOptions"); !object) {
      return std::unexpected(std::move(object.error()));
    }

    auto cwd = terminal_detail::get(env, value, "cwd");
    if (!cwd) return std::unexpected(std::move(cwd.error()));
    if (!terminal_detail::nullish(cwd->get())) {
      auto decoded = Converter<std::string>::from_js(env, cwd->get());
      if (!decoded) return std::unexpected(Error::type("TerminalOptions.cwd must be a string"));
      result.options.cwd = std::move(*decoded);
    }

    auto environment = terminal_detail::get(env, value, "environment");
    if (!environment) return std::unexpected(std::move(environment.error()));
    auto decoded_environment =
        terminal_detail::environment_from_js(env, environment->get());
    if (!decoded_environment) {
      return std::unexpected(std::move(decoded_environment.error()));
    }
    result.options.environment = std::move(*decoded_environment);

    auto columns = terminal_detail::get(env, value, "columns");
    if (!columns) return std::unexpected(std::move(columns.error()));
    if (!terminal_detail::nullish(columns->get())) {
      auto decoded = terminal_detail::bounded<std::uint16_t>(
          env, columns->get(), "TerminalOptions.columns", 2, 500);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.columns = *decoded;
    }

    auto rows = terminal_detail::get(env, value, "rows");
    if (!rows) return std::unexpected(std::move(rows.error()));
    if (!terminal_detail::nullish(rows->get())) {
      auto decoded = terminal_detail::bounded<std::uint16_t>(
          env, rows->get(), "TerminalOptions.rows", 1, 200);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.rows = *decoded;
    }

    auto timeout = terminal_detail::get(env, value, "timeoutMs");
    if (!timeout) return std::unexpected(std::move(timeout.error()));
    if (!terminal_detail::nullish(timeout->get())) {
      auto decoded = terminal_detail::bounded<std::uint64_t>(
          env, timeout->get(), "TerminalOptions.timeoutMs", 0, 3'600'000);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.timeout = std::chrono::milliseconds{*decoded};
    }

    auto buffer = terminal_detail::get(env, value, "maxBufferBytes");
    if (!buffer) return std::unexpected(std::move(buffer.error()));
    if (!terminal_detail::nullish(buffer->get())) {
      auto decoded = terminal_detail::bounded<std::size_t>(
          env, buffer->get(), "TerminalOptions.maxBufferBytes", 65'536, 16 * 1024 * 1024);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.max_buffer_bytes = *decoded;
    }
    return result;
  }
};

template <>
struct Converter<std::optional<tm::OptionsDecode>> {
  static Result<std::optional<tm::OptionsDecode>> from_js(
      Env env,
      JSValueConst value) {
    if (terminal_detail::nullish(value)) {
      return std::optional<tm::OptionsDecode>{};
    }
    auto decoded = Converter<tm::OptionsDecode>::from_js(env, value);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return std::optional{std::move(*decoded)};
  }
};

template <>
struct Converter<tm::WritePayload> {
  static Result<tm::WritePayload> from_js(Env env, JSValueConst value) {
    auto bytes = Converter<std::vector<std::uint8_t>>::from_js(env, value);
    if (!bytes) return std::unexpected(std::move(bytes.error()));
    if (bytes->size() > 1024 * 1024) {
      return std::unexpected(Error::range("Terminal.write payload exceeds 1 MiB"));
    }
    return tm::WritePayload{.data = std::move(*bytes)};
  }
};

template <>
struct Converter<tm::ReadOptionsDecode> {
  static Result<tm::ReadOptionsDecode> from_js(Env env, JSValueConst value) {
    tm::ReadOptionsDecode result;
    if (terminal_detail::nullish(value)) return result;
    if (auto object = terminal_detail::require_object(env, value, "TerminalReadOptions"); !object) {
      return std::unexpected(std::move(object.error()));
    }
    auto offset = terminal_detail::get(env, value, "offset");
    if (!offset) return std::unexpected(std::move(offset.error()));
    if (!terminal_detail::nullish(offset->get())) {
      auto decoded = terminal_detail::bounded<std::uint64_t>(
          env, offset->get(), "TerminalReadOptions.offset", 0, 9'007'199'254'740'991ull);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.offset = *decoded;
    }
    auto max = terminal_detail::get(env, value, "maxBytes");
    if (!max) return std::unexpected(std::move(max.error()));
    if (!terminal_detail::nullish(max->get())) {
      auto decoded = terminal_detail::bounded<std::size_t>(
          env, max->get(), "TerminalReadOptions.maxBytes", 4, 1024 * 1024);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.max_bytes = *decoded;
    }
    auto wait = terminal_detail::get(env, value, "waitMs");
    if (!wait) return std::unexpected(std::move(wait.error()));
    if (!terminal_detail::nullish(wait->get())) {
      auto decoded = terminal_detail::bounded<std::uint64_t>(
          env, wait->get(), "TerminalReadOptions.waitMs", 0, 60'000);
      if (!decoded) return std::unexpected(std::move(decoded.error()));
      result.options.wait = std::chrono::milliseconds{*decoded};
    }
    return result;
  }
};

template <>
struct Converter<tm::Columns> {
  static Result<tm::Columns> from_js(Env env, JSValueConst value) {
    auto decoded = terminal_detail::bounded<std::uint16_t>(env, value, "columns", 2, 500);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return tm::Columns{.value = *decoded};
  }
};

template <>
struct Converter<tm::Rows> {
  static Result<tm::Rows> from_js(Env env, JSValueConst value) {
    auto decoded = terminal_detail::bounded<std::uint16_t>(env, value, "rows", 1, 200);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return tm::Rows{.value = *decoded};
  }
};

template <>
struct Converter<tm::Signal> {
  static Result<tm::Signal> from_js(Env env, JSValueConst value) {
    auto name = Converter<std::string>::from_js(env, value);
    if (!name) return std::unexpected(Error::type("signal must be a string"));
    auto signal = terminal::decode_signal(*name);
    if (!signal) return std::unexpected(Error::range(std::move(signal.error().message)));
    return tm::Signal{.value = *signal};
  }
};

template <>
struct Converter<tm::WaitTimeout> {
  static Result<tm::WaitTimeout> from_js(Env env, JSValueConst value) {
    if (terminal_detail::nullish(value)) return tm::WaitTimeout{};
    auto decoded = terminal_detail::bounded<std::uint64_t>(
        env, value, "timeoutMs", 0, 60'000);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return tm::WaitTimeout{.value = std::chrono::milliseconds{*decoded}};
  }
};

template <>
struct Converter<tm::CloseGrace> {
  static Result<tm::CloseGrace> from_js(Env env, JSValueConst value) {
    if (terminal_detail::nullish(value)) return tm::CloseGrace{};
    auto decoded = terminal_detail::bounded<std::uint64_t>(
        env, value, "gracePeriodMs", 0, 60'000);
    if (!decoded) return std::unexpected(std::move(decoded.error()));
    return tm::CloseGrace{.value = std::chrono::milliseconds{*decoded}};
  }
};

template <>
struct Converter<terminal::ReadResult> {
  static qjs::OwnedValue to_js(Env env, terminal::ReadResult result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception() ||
        !terminal_detail::set_exit(env, object.get(), result.exit) ||
        !terminal_detail::set(
            env,
            object.get(),
            "data",
            Converter<std::vector<std::uint8_t>>::to_js(env, std::move(result.data))) ||
        !terminal_detail::set(
            env,
            object.get(),
            "nextOffset",
            Converter<std::uint64_t>::to_js(env, result.next_offset)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "availableFrom",
            Converter<std::uint64_t>::to_js(env, result.available_from)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "droppedBytes",
            Converter<std::uint64_t>::to_js(env, result.dropped_bytes)) ||
        !terminal_detail::set(env, object.get(), "dropped", env.boolean(result.dropped)) ||
        !terminal_detail::set(env, object.get(), "eof", env.boolean(result.eof))) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }
};

template <>
struct Converter<terminal::CloseResult> {
  static qjs::OwnedValue to_js(Env env, const terminal::CloseResult& result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception() ||
        !terminal_detail::set_exit(env, object.get(), result.exit) ||
        !terminal_detail::set(
            env, object.get(), "escalated", env.boolean(result.escalated)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "finalSignal",
            result.final_signal
                ? env.string(terminal_detail::signal_name(*result.final_signal))
                : env.null_value())) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }
};

template <>
struct Converter<terminal::ExitWaitResult> {
  static qjs::OwnedValue to_js(Env env, const terminal::ExitWaitResult& result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception() ||
        !terminal_detail::set_exit(env, object.get(), result.exit) ||
        !terminal_detail::set(env, object.get(), "completed", env.boolean(result.completed))) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }
};

template <>
struct Converter<terminal::TerminalSnapshot> {
  static qjs::OwnedValue to_js(Env env, const terminal::TerminalSnapshot& result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception() ||
        !terminal_detail::set_exit(env, object.get(), result.exit) ||
        !terminal_detail::set(env, object.get(), "columns", env.uint32(result.columns)) ||
        !terminal_detail::set(env, object.get(), "rows", env.uint32(result.rows)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "eraseCharacter",
            env.uint32(result.erase_character)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "nextOffset",
            Converter<std::uint64_t>::to_js(env, result.next_offset)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "availableFrom",
            Converter<std::uint64_t>::to_js(env, result.available_from)) ||
        !terminal_detail::set(
            env,
            object.get(),
            "bufferedBytes",
            Converter<std::size_t>::to_js(env, result.buffered_bytes))) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }
};

}  // namespace vacps::binding
