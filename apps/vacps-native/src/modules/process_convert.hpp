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

}  // namespace vacps::js::process_module

namespace vacps::binding {

namespace process_module = vacps::js::process_module;

namespace process_detail {

inline constexpr std::int64_t k_min_timeout_ms = 0;
inline constexpr std::int64_t k_max_timeout_ms = 3'600'000;  // 1 hour

inline constexpr std::uint64_t k_min_max_bytes = 0;
inline constexpr std::uint64_t k_max_max_bytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_default_max_bytes = 16ull * 1024ull * 1024ull;

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

}  // namespace vacps::binding
