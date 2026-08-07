#pragma once

/**
 * Module-local JS ↔ C++ convert for vacps:process.
 *
 * ProcessOptions (honest, narrow):
 *   cwd?, timeoutMs?, stdin?: 'pipe'|'ignore', maxStdoutBytes?, maxStderrBytes?
 * Rejects env and any other unsupported keys that would invent API fiction
 * (env is explicitly rejected when supplied). stdout/stderr mode keys rejected.
 * No hardMax* / closeStdin aliases.
 *
 * stdin defaults are applied by the caller (Process class → pipe/open;
 * run → ignore/closed) when the property is omitted.
 *
 * ProcessResult → captured strings plus exact drained-byte/truncation facts.
 */

#include "binding/convert.hpp"
#include "process/process.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js::process_module {

/** Decoded options plus whether stdin was explicitly supplied. */
struct ProcessOptionsDecode {
  vacps::process::StartOptions opts{};
  bool stdin_specified{false};
};

/** Validated terminate signal (domain signo). */
struct TerminateSignal {
  int signo{SIGTERM};
};

/** Optional string args array (missing/null/undefined → empty). */
struct OptionalStringArgs {
  std::vector<std::string> args;
};

/** Write payload: string | ArrayBuffer | TypedArray → bytes as std::string. */
struct WritePayload {
  std::string data;
};

struct CloseStdin {
  bool value{false};
};

struct GracePeriod {
  std::chrono::milliseconds value{3000};
};

struct ExitWait {
  std::optional<std::chrono::milliseconds> timeout;
};

struct ReadOptionsDecode {
  vacps::process::ReadOptions options;
};

struct SnapshotOptionsDecode {
  std::size_t stdout_bytes{16 * 1024};
  std::size_t stderr_bytes{16 * 1024};
};

}  // namespace vacps::js::process_module

namespace vacps::binding {

namespace process_module = vacps::js::process_module;

namespace process_detail {

inline constexpr std::int64_t k_min_timeout_ms = 0;
inline constexpr std::int64_t k_max_timeout_ms = 3'600'000;  // 1 hour

inline constexpr std::uint64_t k_min_max_bytes = 0;
inline constexpr std::uint64_t k_max_max_bytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_default_max_bytes = 16ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_max_protocol_read_bytes = 1024ull * 1024ull;
inline constexpr std::int64_t k_max_read_wait_ms = 60'000;
inline constexpr std::uint64_t k_max_safe_integer = 9'007'199'254'740'991ull;

[[nodiscard]] inline bool is_nullish(JSValueConst v) noexcept {
  return JS_IsUndefined(v) || JS_IsNull(v);
}

[[nodiscard]] inline Result<qjs::OwnedValue> get_prop(
    Env env,
    JSValueConst obj,
    const char* name) {
  JSContext* ctx = env.context();
  qjs::OwnedValue v = qjs::OwnedValue::get_property_str(ctx, obj, name);
  if (v.is_exception()) {
    clear_exception(ctx);
    (void)v.release();
    return std::unexpected(Error::type(
        std::string{"failed to read property '"} + name + "'"));
  }
  return v;
}

[[nodiscard]] inline Result<void> require_plain_object(
    Env env,
    JSValueConst v,
    const char* label) {
  JSContext* ctx = env.context();
  if (!JS_IsObject(v)) {
    return std::unexpected(Error::type(
        std::string{label} + " must be a plain object"));
  }
  const int is_arr = JS_IsArray(ctx, v);
  if (is_arr < 0) {
    clear_exception(ctx);
    return std::unexpected(Error::type(
        std::string{label} + " must be a plain object"));
  }
  if (is_arr != 0) {
    return std::unexpected(Error::type(
        std::string{label} + " must be a plain object"));
  }
  return {};
}

[[nodiscard]] inline Result<std::uint64_t> bounded_u64(
    Env env,
    JSValueConst v,
    const char* label,
    std::uint64_t lo,
    std::uint64_t hi) {
  auto n = Converter<std::uint64_t>::from_js(env, v);
  if (!n) {
    if (n.error().kind == ErrorKind::type) {
      return std::unexpected(Error::type(
          std::string{label} + " must be an integer"));
    }
    return std::unexpected(std::move(n.error()));
  }
  if (*n < lo || *n > hi) {
    return std::unexpected(Error::range(std::format(
        "{} must be an integer in [{}, {}]", label, lo, hi)));
  }
  return *n;
}

[[nodiscard]] inline Result<std::uint32_t> array_length(
    Env env,
    JSValueConst arr,
    const char* label) {
  auto len_v = get_prop(env, arr, "length");
  if (!len_v) {
    return std::unexpected(std::move(len_v.error()));
  }
  auto n = Converter<std::uint32_t>::from_js(env, len_v->get());
  if (!n) {
    return std::unexpected(Error::type(
        std::string{label} + ".length must be a non-negative integer"));
  }
  return *n;
}

[[nodiscard]] inline Result<std::vector<std::string>> string_array_from_js(
    Env env,
    JSValueConst v,
    const char* label) {
  JSContext* ctx = env.context();
  if (is_nullish(v)) {
    return std::vector<std::string>{};
  }
  const int is_arr = JS_IsArray(ctx, v);
  if (is_arr < 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type(std::string{label} + " must be an array of strings"));
  }
  if (is_arr == 0) {
    return std::unexpected(
        Error::type(std::string{label} + " must be an array of strings"));
  }
  auto len = array_length(env, v, label);
  if (!len) {
    return std::unexpected(std::move(len.error()));
  }
  std::vector<std::string> out;
  out.reserve(*len);
  for (std::uint32_t i = 0; i < *len; ++i) {
    qjs::OwnedValue el{ctx, JS_GetPropertyUint32(ctx, v, i)};
    if (el.is_exception()) {
      clear_exception(ctx);
      (void)el.release();
      return std::unexpected(Error::type(
          std::string{label} + " must be an array of strings"));
    }
    auto s = Converter<std::string>::from_js(env, el.get());
    if (!s) {
      return std::unexpected(Error::type(
          std::string{label} + "[" + std::to_string(i) +
          "] must be a string"));
    }
    out.push_back(std::move(*s));
  }
  return out;
}

[[nodiscard]] inline std::string_view status_name(
    vacps::process::ProcessStatus status) noexcept {
  using vacps::process::ProcessStatus;
  switch (status) {
    case ProcessStatus::Created:
    case ProcessStatus::Starting:
    case ProcessStatus::Running:
      return "running";
    case ProcessStatus::Exited:
      return "exited";
    case ProcessStatus::Signaled:
      return "signaled";
    case ProcessStatus::TimedOut:
      return "timed_out";
    case ProcessStatus::Cancelled:
    case ProcessStatus::Closing:
    case ProcessStatus::Closed:
      return "cancelled";
  }
  return "cancelled";
}

[[nodiscard]] inline std::string signal_name(int signal) {
  switch (signal) {
    case SIGTERM:
      return "SIGTERM";
    case SIGINT:
      return "SIGINT";
    case SIGKILL:
      return "SIGKILL";
    default:
      return std::format("SIG{}", signal);
  }
}

[[nodiscard]] inline bool set_property(
    Env env,
    JSValueConst object,
    const char* key,
    qjs::OwnedValue value) {
  if (value.is_exception()) {
    return false;
  }
  return JS_SetPropertyStr(env.context(), object, key, value.release()) >= 0;
}

[[nodiscard]] inline bool set_exit_properties(
    Env env,
    JSValueConst object,
    const vacps::process::ExitResult& exit) {
  if (!set_property(
          env,
          object,
          "status",
          env.string(status_name(exit.status)))) {
    return false;
  }
  if (!set_property(
          env,
          object,
          "exitCode",
          exit.exit_code.has_value()
              ? Converter<std::int32_t>::to_js(env, *exit.exit_code)
              : env.null_value())) {
    return false;
  }
  if (!set_property(
          env,
          object,
          "signal",
          exit.signal.has_value()
              ? env.string(signal_name(*exit.signal))
              : env.null_value())) {
    return false;
  }
  return set_property(
      env, object, "timedOut", Converter<bool>::to_js(env, exit.timed_out));
}

}  // namespace process_detail

template <>
struct Converter<vacps::js::process_module::OptionalStringArgs> {
  static Result<vacps::js::process_module::OptionalStringArgs> from_js(
      Env env,
      JSValueConst v) {
    auto args = process_detail::string_array_from_js(env, v, "args");
    if (!args) {
      return std::unexpected(std::move(args.error()));
    }
    return vacps::js::process_module::OptionalStringArgs{std::move(*args)};
  }
};

template <>
struct Converter<vacps::js::process_module::ProcessOptionsDecode> {
  static Result<vacps::js::process_module::ProcessOptionsDecode> from_js(
      Env env,
      JSValueConst v) {
    vacps::js::process_module::ProcessOptionsDecode out;
    out.opts.max_stdout_bytes =
        static_cast<std::size_t>(process_detail::k_default_max_bytes);
    out.opts.max_stderr_bytes =
        static_cast<std::size_t>(process_detail::k_default_max_bytes);

    if (process_detail::is_nullish(v)) {
      return out;
    }
    if (auto ok = process_detail::require_plain_object(env, v, "options"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    // env — not supported; reject when supplied (including null).
    {
      auto env_v = process_detail::get_prop(env, v, "env");
      if (!env_v) {
        return std::unexpected(std::move(env_v.error()));
      }
      if (!process_detail::is_nullish(env_v->get())) {
        return std::unexpected(Error::type(
            "ProcessOptions.env is not supported"));
      }
    }

    // stdout / stderr mode fiction — reject when supplied.
    for (const char* key : {"stdout", "stderr"}) {
      auto mode_v = process_detail::get_prop(env, v, key);
      if (!mode_v) {
        return std::unexpected(std::move(mode_v.error()));
      }
      if (!process_detail::is_nullish(mode_v->get())) {
        return std::unexpected(Error::type(
            std::string{"ProcessOptions."} + key +
            " is not supported (stdout/stderr are always captured pipes)"));
      }
    }

    // cwd?: string
    {
      auto cwd_v = process_detail::get_prop(env, v, "cwd");
      if (!cwd_v) {
        return std::unexpected(std::move(cwd_v.error()));
      }
      if (!process_detail::is_nullish(cwd_v->get())) {
        auto cwd = Converter<std::string>::from_js(env, cwd_v->get());
        if (!cwd) {
          return std::unexpected(Error::type("ProcessOptions.cwd must be a string"));
        }
        out.opts.cwd = std::move(*cwd);
      }
    }

    // timeoutMs?: integer >= 0 (0 = none)
    {
      auto t_v = process_detail::get_prop(env, v, "timeoutMs");
      if (!t_v) {
        return std::unexpected(std::move(t_v.error()));
      }
      if (!process_detail::is_nullish(t_v->get())) {
        auto ms = Converter<std::int64_t>::from_js(env, t_v->get());
        if (!ms) {
          return std::unexpected(
              Error::type("ProcessOptions.timeoutMs must be an integer"));
        }
        if (*ms < process_detail::k_min_timeout_ms ||
            *ms > process_detail::k_max_timeout_ms) {
          return std::unexpected(Error::range(std::format(
              "ProcessOptions.timeoutMs must be an integer in [{}, {}]",
              process_detail::k_min_timeout_ms,
              process_detail::k_max_timeout_ms)));
        }
        out.opts.timeout = std::chrono::milliseconds{*ms};
      }
    }

    // stdin?: 'pipe' | 'ignore'
    {
      auto s_v = process_detail::get_prop(env, v, "stdin");
      if (!s_v) {
        return std::unexpected(std::move(s_v.error()));
      }
      if (!process_detail::is_nullish(s_v->get())) {
        auto mode = Converter<std::string>::from_js(env, s_v->get());
        if (!mode) {
          return std::unexpected(
              Error::type("ProcessOptions.stdin must be 'pipe' or 'ignore'"));
        }
        if (*mode == "pipe") {
          out.opts.close_stdin = false;
          out.stdin_specified = true;
        } else if (*mode == "ignore") {
          out.opts.close_stdin = true;
          out.stdin_specified = true;
        } else {
          return std::unexpected(
              Error::type("ProcessOptions.stdin must be 'pipe' or 'ignore'"));
        }
      }
    }

    // maxStdoutBytes?: 0..64MiB
    {
      auto m_v = process_detail::get_prop(env, v, "maxStdoutBytes");
      if (!m_v) {
        return std::unexpected(std::move(m_v.error()));
      }
      if (!process_detail::is_nullish(m_v->get())) {
        auto n = process_detail::bounded_u64(
            env,
            m_v->get(),
            "ProcessOptions.maxStdoutBytes",
            process_detail::k_min_max_bytes,
            process_detail::k_max_max_bytes);
        if (!n) {
          return std::unexpected(std::move(n.error()));
        }
        out.opts.max_stdout_bytes = static_cast<std::size_t>(*n);
      }
    }

    // maxStderrBytes?: 0..64MiB
    {
      auto m_v = process_detail::get_prop(env, v, "maxStderrBytes");
      if (!m_v) {
        return std::unexpected(std::move(m_v.error()));
      }
      if (!process_detail::is_nullish(m_v->get())) {
        auto n = process_detail::bounded_u64(
            env,
            m_v->get(),
            "ProcessOptions.maxStderrBytes",
            process_detail::k_min_max_bytes,
            process_detail::k_max_max_bytes);
        if (!n) {
          return std::unexpected(std::move(n.error()));
        }
        out.opts.max_stderr_bytes = static_cast<std::size_t>(*n);
      }
    }

    return out;
  }
};

/** Optional trailing ProcessOptions (missing/null/undefined → defaults). */
template <>
struct Converter<std::optional<vacps::js::process_module::ProcessOptionsDecode>> {
  static Result<std::optional<vacps::js::process_module::ProcessOptionsDecode>>
  from_js(Env env, JSValueConst v) {
    if (process_detail::is_nullish(v)) {
      return std::optional<vacps::js::process_module::ProcessOptionsDecode>{};
    }
    auto d =
        Converter<vacps::js::process_module::ProcessOptionsDecode>::from_js(
            env, v);
    if (!d) {
      return std::unexpected(std::move(d.error()));
    }
    return std::optional{*d};
  }
};

template <>
struct Converter<vacps::js::process_module::TerminateSignal> {
  static Result<vacps::js::process_module::TerminateSignal> from_js(
      Env env,
      JSValueConst v) {
    if (process_detail::is_nullish(v)) {
      return vacps::js::process_module::TerminateSignal{};
    }
    auto s = Converter<std::string>::from_js(env, v);
    if (!s) {
      return std::unexpected(Error::type(
          "signal must be 'SIGTERM', 'SIGINT', or 'SIGKILL'"));
    }
    auto decoded = vacps::process::decode_terminate_signal(*s);
    if (!decoded) {
      return std::unexpected(Error::type(std::move(decoded.error().message)));
    }
    return vacps::js::process_module::TerminateSignal{*decoded};
  }
};

template <>
struct Converter<vacps::js::process_module::WritePayload> {
  static Result<vacps::js::process_module::WritePayload> from_js(
      Env env,
      JSValueConst v) {
    auto bytes = Converter<std::vector<std::uint8_t>>::from_js(env, v);
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    vacps::js::process_module::WritePayload out;
    out.data.assign(
        reinterpret_cast<const char*>(bytes->data()), bytes->size());
    return out;
  }
};

template <>
struct Converter<vacps::js::process_module::CloseStdin> {
  static Result<vacps::js::process_module::CloseStdin> from_js(
      Env env,
      JSValueConst v) {
    if (process_detail::is_nullish(v)) {
      return vacps::js::process_module::CloseStdin{};
    }
    auto close = Converter<bool>::from_js(env, v);
    if (!close) {
      return std::unexpected(Error::type("closeStdin must be a boolean"));
    }
    return vacps::js::process_module::CloseStdin{*close};
  }
};

template <>
struct Converter<vacps::js::process_module::GracePeriod> {
  static Result<vacps::js::process_module::GracePeriod> from_js(
      Env env,
      JSValueConst v) {
    if (process_detail::is_nullish(v)) {
      return vacps::js::process_module::GracePeriod{};
    }
    auto ms = process_detail::bounded_u64(
        env, v, "gracePeriodMs", 0, process_detail::k_max_read_wait_ms);
    if (!ms) {
      return std::unexpected(std::move(ms.error()));
    }
    return vacps::js::process_module::GracePeriod{
        std::chrono::milliseconds{static_cast<std::int64_t>(*ms)}};
  }
};

template <>
struct Converter<vacps::js::process_module::ExitWait> {
  static Result<vacps::js::process_module::ExitWait> from_js(
      Env env,
      JSValueConst v) {
    if (process_detail::is_nullish(v)) {
      return vacps::js::process_module::ExitWait{};
    }
    auto ms = process_detail::bounded_u64(
        env, v, "timeoutMs", 0, process_detail::k_max_timeout_ms);
    if (!ms) {
      return std::unexpected(std::move(ms.error()));
    }
    return vacps::js::process_module::ExitWait{
        std::chrono::milliseconds{static_cast<std::int64_t>(*ms)}};
  }
};

template <>
struct Converter<vacps::js::process_module::ReadOptionsDecode> {
  static Result<vacps::js::process_module::ReadOptionsDecode> from_js(
      Env env,
      JSValueConst v) {
    vacps::js::process_module::ReadOptionsDecode out;
    if (process_detail::is_nullish(v)) {
      return out;
    }
    if (auto ok = process_detail::require_plain_object(env, v, "read options"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    auto sequence_v = process_detail::get_prop(env, v, "sequence");
    if (!sequence_v) {
      return std::unexpected(std::move(sequence_v.error()));
    }
    if (!process_detail::is_nullish(sequence_v->get())) {
      auto sequence = process_detail::bounded_u64(
          env,
          sequence_v->get(),
          "sequence",
          1,
          process_detail::k_max_safe_integer);
      if (!sequence) {
        return std::unexpected(std::move(sequence.error()));
      }
      out.options.cursor.sequence = *sequence;
    }

    auto offset_v = process_detail::get_prop(env, v, "byteOffset");
    if (!offset_v) {
      return std::unexpected(std::move(offset_v.error()));
    }
    if (!process_detail::is_nullish(offset_v->get())) {
      auto offset = process_detail::bounded_u64(
          env,
          offset_v->get(),
          "byteOffset",
          0,
          process_detail::k_max_protocol_read_bytes);
      if (!offset) {
        return std::unexpected(std::move(offset.error()));
      }
      out.options.cursor.byte_offset = static_cast<std::size_t>(*offset);
    }

    auto max_v = process_detail::get_prop(env, v, "maxBytes");
    if (!max_v) {
      return std::unexpected(std::move(max_v.error()));
    }
    if (!process_detail::is_nullish(max_v->get())) {
      auto max_bytes = process_detail::bounded_u64(
          env,
          max_v->get(),
          "maxBytes",
          1,
          process_detail::k_max_protocol_read_bytes);
      if (!max_bytes) {
        return std::unexpected(std::move(max_bytes.error()));
      }
      out.options.max_bytes = static_cast<std::size_t>(*max_bytes);
    }

    auto wait_v = process_detail::get_prop(env, v, "waitMs");
    if (!wait_v) {
      return std::unexpected(std::move(wait_v.error()));
    }
    if (!process_detail::is_nullish(wait_v->get())) {
      auto wait = process_detail::bounded_u64(
          env,
          wait_v->get(),
          "waitMs",
          0,
          process_detail::k_max_read_wait_ms);
      if (!wait) {
        return std::unexpected(std::move(wait.error()));
      }
      out.options.wait =
          std::chrono::milliseconds{static_cast<std::int64_t>(*wait)};
    }

    return out;
  }
};

template <>
struct Converter<vacps::js::process_module::SnapshotOptionsDecode> {
  static Result<vacps::js::process_module::SnapshotOptionsDecode> from_js(
      Env env,
      JSValueConst v) {
    vacps::js::process_module::SnapshotOptionsDecode out;
    if (process_detail::is_nullish(v)) {
      return out;
    }
    if (auto ok = process_detail::require_plain_object(env, v, "snapshot options"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    auto decode_limit = [env, v](
                            const char* key,
                            std::size_t& target) -> Result<void> {
      auto value = process_detail::get_prop(env, v, key);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      if (process_detail::is_nullish(value->get())) {
        return {};
      }
      auto bytes = process_detail::bounded_u64(
          env,
          value->get(),
          key,
          0,
          process_detail::k_max_protocol_read_bytes);
      if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
      }
      target = static_cast<std::size_t>(*bytes);
      return {};
    };

    if (auto decoded = decode_limit("stdoutMaxBytes", out.stdout_bytes); !decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    if (auto decoded = decode_limit("stderrMaxBytes", out.stderr_bytes); !decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    return out;
  }
};

template <>
struct Converter<vacps::process::RunResult> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::process::RunResult& r) {
    JSContext* ctx = env.context();
    auto obj = env.new_object();
    if (obj.is_exception()) {
      return obj;
    }

    auto set = [&](const char* key, qjs::OwnedValue val) -> bool {
      if (val.is_exception()) {
        return false;
      }
      if (JS_SetPropertyStr(ctx, obj.get(), key, val.release()) < 0) {
        return false;
      }
      return true;
    };

    if (!set("exitCode", Converter<std::int32_t>::to_js(env, r.exit_code))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("timedOut", Converter<bool>::to_js(env, r.timed_out))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("stdout", Converter<std::string>::to_js(env, r.stdout_str))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("stderr", Converter<std::string>::to_js(env, r.stderr_str))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "stdoutBytes",
            Converter<std::uint64_t>::to_js(env, r.stdout_bytes))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "stderrBytes",
            Converter<std::uint64_t>::to_js(env, r.stderr_bytes))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "stdoutTruncated",
            Converter<bool>::to_js(env, r.stdout_truncated))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "stderrTruncated",
            Converter<bool>::to_js(env, r.stderr_truncated))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::process::RunResult&& r) {
    return to_js(env, static_cast<const vacps::process::RunResult&>(r));
  }
};

template <>
struct Converter<vacps::process::ExitResult> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::process::ExitResult& result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception()) {
      return object;
    }
    if (!process_detail::set_exit_properties(env, object.get(), result)) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }

  static qjs::OwnedValue to_js(Env env, vacps::process::ExitResult&& result) {
    return to_js(env, static_cast<const vacps::process::ExitResult&>(result));
  }
};

template <>
struct Converter<vacps::process::ExitWaitResult> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::process::ExitWaitResult& result) {
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception()) {
      return object;
    }
    if (!process_detail::set_exit_properties(env, object.get(), result.exit) ||
        !process_detail::set_property(
            env,
            object.get(),
            "completed",
            env.boolean(result.completed))) {
      return qjs::OwnedValue::take(env.context(), JS_EXCEPTION);
    }
    return object;
  }

  static qjs::OwnedValue to_js(
      Env env,
      vacps::process::ExitWaitResult&& result) {
    return to_js(env, static_cast<const vacps::process::ExitWaitResult&>(result));
  }
};

template <>
struct Converter<vacps::process::ReadResult> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::process::ReadResult& result) {
    JSContext* ctx = env.context();
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception()) {
      return object;
    }
    if (!process_detail::set_exit_properties(env, object.get(), result.exit)) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    qjs::OwnedValue chunks = env.new_array();
    if (chunks.is_exception()) {
      return chunks;
    }
    for (std::size_t i = 0; i < result.chunks.size(); ++i) {
      const vacps::process::OutputChunk& chunk = result.chunks[i];
      qjs::OwnedValue item = env.new_object();
      if (item.is_exception()) {
        return item;
      }
      const bool encoded =
          process_detail::set_property(
              env,
              item.get(),
              "sequence",
              env.float64(static_cast<double>(chunk.sequence))) &&
          process_detail::set_property(
              env,
              item.get(),
              "stream",
              env.string(
                  chunk.stream == vacps::process::ProcessStream::Stdout
                      ? "stdout"
                      : "stderr")) &&
          process_detail::set_property(
              env, item.get(), "data", env.string(chunk.data)) &&
          process_detail::set_property(
              env,
              item.get(),
              "observedAtMs",
              env.float64(static_cast<double>(chunk.observed_at_ms))) &&
          process_detail::set_property(
              env,
              item.get(),
              "offsetStart",
              env.float64(static_cast<double>(chunk.offset_start))) &&
          process_detail::set_property(
              env,
              item.get(),
              "offsetEnd",
              env.float64(static_cast<double>(chunk.offset_end)));
      if (!encoded ||
          JS_SetPropertyUint32(
              ctx, chunks.get(), static_cast<std::uint32_t>(i), item.release()) < 0) {
        return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
      }
    }

    const bool encoded =
        process_detail::set_property(
            env, object.get(), "chunks", std::move(chunks)) &&
        process_detail::set_property(
            env,
            object.get(),
            "nextSequence",
            env.float64(static_cast<double>(result.next_cursor.sequence))) &&
        process_detail::set_property(
            env,
            object.get(),
            "nextByteOffset",
            env.float64(static_cast<double>(result.next_cursor.byte_offset))) &&
        process_detail::set_property(
            env, object.get(), "eof", env.boolean(result.eof)) &&
        process_detail::set_property(
            env,
            object.get(),
            "returnedBytes",
            env.float64(static_cast<double>(result.returned_bytes)));
    if (!encoded) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return object;
  }

  static qjs::OwnedValue to_js(Env env, vacps::process::ReadResult&& result) {
    return to_js(env, static_cast<const vacps::process::ReadResult&>(result));
  }
};

template <>
struct Converter<vacps::process::ProcessSnapshot> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::process::ProcessSnapshot& snapshot) {
    JSContext* ctx = env.context();
    qjs::OwnedValue object = env.new_object();
    if (object.is_exception()) {
      return object;
    }
    const bool encoded =
        process_detail::set_exit_properties(env, object.get(), snapshot.exit) &&
        process_detail::set_property(
            env,
            object.get(),
            "stdinAvailable",
            env.boolean(snapshot.stdin_available)) &&
        process_detail::set_property(
            env, object.get(), "stdout", env.string(snapshot.stdout_str)) &&
        process_detail::set_property(
            env, object.get(), "stderr", env.string(snapshot.stderr_str)) &&
        process_detail::set_property(
            env,
            object.get(),
            "stdoutBytes",
            env.float64(static_cast<double>(snapshot.stdout_bytes))) &&
        process_detail::set_property(
            env,
            object.get(),
            "stderrBytes",
            env.float64(static_cast<double>(snapshot.stderr_bytes))) &&
        process_detail::set_property(
            env,
            object.get(),
            "stdoutTruncated",
            env.boolean(snapshot.stdout_truncated)) &&
        process_detail::set_property(
            env,
            object.get(),
            "stderrTruncated",
            env.boolean(snapshot.stderr_truncated));
    if (!encoded) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return object;
  }

  static qjs::OwnedValue to_js(
      Env env,
      vacps::process::ProcessSnapshot&& snapshot) {
    return to_js(env, static_cast<const vacps::process::ProcessSnapshot&>(snapshot));
  }
};

}  // namespace vacps::binding
