#pragma once

/**
 * Module-local JS ↔ C++ convert for vacps:store (ClassBuilder).
 *
 * Specializes binding::Converter for storage domain types used by the store
 * binding. QueryOptions lives in vacps::js::store_module (binding surface only).
 *
 * Contract (matches binding::Converter):
 * - from_js borrows JSValueConst; never frees/retains the argument.
 * - On from_js failure, clears any pending QuickJS exception and returns
 *   binding::Error (engine left clean — Result contract).
 * - to_js returns caller-owned qjs::OwnedValue.
 * - On to_js failure, returns qjs::OwnedValue(JS_EXCEPTION) with exactly one
 *   pending QuickJS exception (the failure cause). Never clears a pending
 *   exception in order to replace it.
 * - Every owned intermediate JSValue is qjs::OwnedValue.
 * - JS_SetProperty* always consumes the value argument (including on failure);
 *   release() before the call — never double-free.
 * - Property getters and array index reads may throw; from_js maps them to
 *   Error with a clean engine state.
 *
 * Fixed query signature is query(sql, params?, options?). There is no
 * query(sql, options) decode path.
 *
 * Encode shapes (product TS):
 * - SqlValue → null | number | bigint | string | ArrayBuffer
 *   (safe-integer int64 → Number; wider signed int64 → BigInt)
 * - RunResult → { changes, lastInsertRowid } (same Number/BigInt rule)
 * - QueryResult → Row[] (row objects keyed by column name)
 * - TransactionResult → RunResult | Row[]
 * - vector<TransactionResult> → TransactionResult[]
 */

#include "binding/convert.hpp"
#include "storage/store.hpp"

#include <quickjs.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vacps::js::store_module {

/**
 * JS-facing query(sql, params?, options?) third argument.
 * Defaults match Database / Store::query. Module-local — not a storage domain
 * type; exists only for the binding surface.
 */
struct QueryOptions {
  std::size_t max_rows{storage::Database::kDefaultMaxQueryRows};
  std::optional<std::size_t> max_bytes{};
};

}  // namespace vacps::js::store_module

namespace vacps::binding {
namespace store_detail {

[[nodiscard]] inline bool is_nullish(JSValueConst v) noexcept {
  return JS_IsUndefined(v) || JS_IsNull(v);
}

/** Property get via OwnedValue; maps getter exceptions to binding::Error. */
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

/** Plain object bag (not null, not array). Clears exception if IsArray fails. */
[[nodiscard]] inline Result<void> require_plain_object(
    Env env,
    JSValueConst v,
    const char* label) {
  JSContext* ctx = env.context();
  if (!JS_IsObject(v)) {
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  const int is_arr = JS_IsArray(ctx, v);
  if (is_arr < 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  if (is_arr != 0) {
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  return {};
}

[[nodiscard]] inline Result<void> require_array(
    Env env,
    JSValueConst v,
    const char* label) {
  JSContext* ctx = env.context();
  const int is_arr = JS_IsArray(ctx, v);
  if (is_arr < 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type(std::string{"expected array ("} + label + ")"));
  }
  if (is_arr == 0) {
    return std::unexpected(
        Error::type(std::string{"expected array ("} + label + ")"));
  }
  return {};
}

/**
 * Nonnegative safe integer → size_t (maxRows / maxBytes / array length).
 * Rejects NaN/∞, non-integers, negatives, and values outside JS safe range.
 */
[[nodiscard]] inline Result<std::size_t> nonnegative_size_from_js(
    Env env,
    JSValueConst v,
    const char* label) {
  auto n = Converter<std::uint64_t>::from_js(env, v);
  if (!n) {
    if (n.error().kind == ErrorKind::type) {
      return std::unexpected(
          Error::type(std::string{"expected number ("} + label + ")"));
    }
    return std::unexpected(Error::range(
        std::string{label} + " must be a nonnegative safe integer"));
  }
  if (*n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(Error::range(std::string{label} + " out of range"));
  }
  return static_cast<std::size_t>(*n);
}

[[nodiscard]] inline Result<std::uint32_t> array_length_from_js(
    Env env,
    JSValueConst arr,
    const char* label) {
  auto len_v = get_prop(env, arr, "length");
  if (!len_v) {
    return std::unexpected(std::move(len_v.error()));
  }
  auto n = nonnegative_size_from_js(env, len_v->get(), label);
  if (!n) {
    return std::unexpected(std::move(n.error()));
  }
  if (*n > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(
        Error::range(std::string{label} + " exceeds UINT32_MAX"));
  }
  return static_cast<std::uint32_t>(*n);
}

/** Blob bytes only — ArrayBuffer or TypedArray. Strings are not blobs. */
[[nodiscard]] inline Result<std::vector<std::uint8_t>> blob_bytes_from_js(
    Env env,
    JSValueConst v) {
  JSContext* ctx = env.context();
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  // Own the ArrayBuffer view returned by GetTypedArrayBuffer.
  qjs::OwnedValue ab{
      ctx,
      JS_GetTypedArrayBuffer(
          ctx, v, &byte_offset, &byte_length, &bytes_per_element)};
  if (ab.is_exception()) {
    clear_exception(ctx);
    (void)ab.release();
    // Fall through to raw ArrayBuffer.
  } else if (!ab.is_undefined() && !ab.is_null()) {
    size_t ab_size = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
    if (ab_ptr != nullptr && byte_offset <= ab_size &&
        byte_length <= ab_size - byte_offset) {
      return std::vector<std::uint8_t>(
          ab_ptr + byte_offset, ab_ptr + byte_offset + byte_length);
    }
    clear_exception(ctx);
    return std::unexpected(
        Error::type("expected ArrayBuffer or TypedArray"));
  }

  size_t size = 0;
  uint8_t* buf = JS_GetArrayBuffer(ctx, &size, v);
  if (buf != nullptr) {
    return std::vector<std::uint8_t>(buf, buf + size);
  }
  clear_exception(ctx);
  return std::unexpected(Error::type("expected ArrayBuffer or TypedArray"));
}

/**
 * Finite JS Number → SqlValue int64 (integral safe integer) or double.
 * Avoids UB around int64 limits: only safe-integer integrals become int64.
 * Rejects NaN/∞ and integral Numbers outside the JS safe-integer range
 * (use BigInt for the rest of signed int64).
 */
[[nodiscard]] inline Result<storage::SqlValue> sql_number_from_js(
    Env env,
    JSValueConst v) {
  JSContext* ctx = env.context();
  auto d = detail::require_finite_number(ctx, v);
  if (!d) {
    if (d.error().kind == ErrorKind::type) {
      return std::unexpected(Error::type("expected number"));
    }
    return std::unexpected(std::move(d.error()));
  }
  if (std::trunc(*d) == *d) {
    if (!detail::in_safe_integer_range(*d)) {
      return std::unexpected(Error::range(
          "integer sql parameter exceeds JS safe integer range"));
    }
    // Safe integers are exactly representable and inside int64_t.
    return storage::sql_int(static_cast<std::int64_t>(*d));
  }
  return storage::sql_real(*d);
}

/**
 * JS BigInt → SqlValue int64 when the value fits signed int64.
 * JS_ToBigInt64 truncates mod 2^64; reconstruct and StrictEq to reject
 * out-of-range magnitudes with RangeError (from_js clears any pending
 * exception on failure paths).
 */
[[nodiscard]] inline Result<storage::SqlValue> sql_bigint_from_js(
    Env env,
    JSValueConst v) {
  JSContext* ctx = env.context();
  std::int64_t n = 0;
  if (JS_ToBigInt64(ctx, &n, v) != 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type("expected bigint fitting signed int64"));
  }
  qjs::OwnedValue truncated{ctx, JS_NewBigInt64(ctx, n)};
  if (truncated.is_exception()) {
    clear_exception(ctx);
    (void)truncated.release();
    return std::unexpected(Error::internal("bigint range check failed"));
  }
  if (!JS_StrictEq(ctx, v, truncated.get())) {
    return std::unexpected(Error::range(
        "bigint sql parameter exceeds signed int64 range"));
  }
  return storage::sql_int(n);
}

/**
 * storage int64 / RunResult integers → JS Number when inside the safe-integer
 * range; otherwise JS BigInt so full SQLite INTEGER / rowid round-trips.
 * On JS_NewBigInt64 failure, returns JS_EXCEPTION with the pending exception.
 */
[[nodiscard]] inline qjs::OwnedValue sql_int64_to_js(Env env, std::int64_t n) {
  JSContext* ctx = env.context();
  if (n >= -detail::k_max_safe_integer && n <= detail::k_max_safe_integer) {
    return env.int64(n);
  }
  return qjs::OwnedValue{ctx, JS_NewBigInt64(ctx, n)};
}

[[nodiscard]] inline Result<storage::OpenMode> open_mode_from_js(
    Env env,
    JSValueConst v) {
  auto s = Converter<std::string>::from_js(env, v);
  if (!s) {
    return std::unexpected(
        Error::type("OpenOptions.mode must be a string"));
  }
  if (*s == "read-only") {
    return storage::OpenMode::ReadOnly;
  }
  if (*s == "read-write") {
    return storage::OpenMode::ReadWrite;
  }
  if (*s == "read-write-create") {
    return storage::OpenMode::ReadWriteCreate;
  }
  return std::unexpected(Error::range(
      "OpenOptions.mode must be 'read-only', 'read-write', or "
      "'read-write-create'"));
}

template <class T>
[[nodiscard]] inline Result<std::vector<T>> vector_from_js_array(
    Env env,
    JSValueConst v,
    const char* label) {
  auto arr_ok = require_array(env, v, label);
  if (!arr_ok) {
    return std::unexpected(std::move(arr_ok.error()));
  }
  const std::string length_label = std::string{label} + ".length";
  auto len = array_length_from_js(env, v, length_label.c_str());
  if (!len) {
    return std::unexpected(std::move(len.error()));
  }
  JSContext* ctx = env.context();
  std::vector<T> out;
  out.reserve(*len);
  for (std::uint32_t i = 0; i < *len; ++i) {
    qjs::OwnedValue elem{ctx, JS_GetPropertyUint32(ctx, v, i)};
    if (elem.is_exception()) {
      clear_exception(ctx);
      (void)elem.release();
      return std::unexpected(Error::type(
          std::string{label} + " element read failed"));
    }
    auto item = Converter<T>::from_js(env, elem.get());
    if (!item) {
      return std::unexpected(std::move(item.error()));
    }
    out.push_back(std::move(*item));
  }
  return out;
}

// ── to_js helpers ──────────────────────────────────────────────────

[[nodiscard]] inline bool fits_uint32(std::size_t n) noexcept {
  return n <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

/**
 * Return the JS_EXCEPTION sentinel after a failed encode step.
 * Preserves any exception already pending on ctx; if none is pending,
 * throws a defensive InternalError so the sentinel is never unpaired.
 */
[[nodiscard]] inline qjs::OwnedValue pending_exception(Env env) {
  JSContext* ctx = env.context();
  // throw_internal preserves an existing QuickJS exception.
  return qjs::OwnedValue::take(
      ctx, throw_internal(ctx, "store encode: SetProperty failed"));
}

[[nodiscard]] inline qjs::OwnedValue throw_encode_range(
    Env env,
    const char* msg) {
  return qjs::OwnedValue::take(
      env.context(), throw_range(env.context(), msg));
}

/**
 * Transfer ownership of `value` into obj[name] via JS_SetPropertyStr.
 * - If value is already JS_EXCEPTION, propagates it (no SetProperty).
 * - On SetProperty failure, QuickJS has consumed (freed) the value — do not
 *   free again. Any existing pending exception is preserved; if none is
 *   pending, a defensive InternalError is thrown before returning the
 *   JS_EXCEPTION sentinel.
 */
[[nodiscard]] inline qjs::OwnedValue set_own_property(
    Env env,
    JSValueConst obj,
    const char* name,
    qjs::OwnedValue value) {
  JSContext* ctx = env.context();
  if (value.is_exception()) {
    return value;
  }
  JSValue raw = value.release();
  // Ownership of raw transfers here (including on failure).
  if (JS_SetPropertyStr(ctx, obj, name, raw) < 0) {
    return pending_exception(env);
  }
  return env.undefined();
}

/**
 * Transfer ownership of `value` into arr[index] via JS_SetPropertyUint32.
 * Same ownership / exception rules as set_own_property.
 */
[[nodiscard]] inline qjs::OwnedValue set_own_index(
    Env env,
    JSValueConst arr,
    std::uint32_t index,
    qjs::OwnedValue value) {
  JSContext* ctx = env.context();
  if (value.is_exception()) {
    return value;
  }
  JSValue raw = value.release();
  // Ownership of raw transfers here (including on failure).
  if (JS_SetPropertyUint32(ctx, arr, index, raw) < 0) {
    return pending_exception(env);
  }
  return env.undefined();
}

}  // namespace store_detail

// ── SqlValue ───────────────────────────────────────────────────────────

template <>
struct Converter<storage::SqlValue> {
  /**
   * Accepts: null, finite JS Number, BigInt in signed int64, string,
   * ArrayBuffer, TypedArray.
   * Rejects undefined, bool, out-of-range BigInt, unsafe integral Number,
   * and all other values.
   */
  static Result<storage::SqlValue> from_js(Env env, JSValueConst v) {
    JSContext* ctx = env.context();
    if (JS_IsNull(v)) {
      return storage::sql_null();
    }
    if (JS_IsUndefined(v)) {
      return std::unexpected(
          Error::type("sql value must not be undefined"));
    }
    if (JS_IsBool(v)) {
      return std::unexpected(
          Error::type("sql value must not be boolean"));
    }
    if (JS_IsBigInt(ctx, v)) {
      return store_detail::sql_bigint_from_js(env, v);
    }
    if (JS_IsNumber(v)) {
      return store_detail::sql_number_from_js(env, v);
    }
    if (JS_IsString(v)) {
      auto s = Converter<std::string>::from_js(env, v);
      if (!s) {
        return std::unexpected(std::move(s.error()));
      }
      return storage::sql_text(std::move(*s));
    }
    // ArrayBuffer / TypedArray → blob (copy).
    auto blob = store_detail::blob_bytes_from_js(env, v);
    if (blob) {
      return storage::sql_blob(std::move(*blob));
    }
    // blob_bytes_from_js already cleared any pending exception.
    return std::unexpected(Error::type(
        "expected null, finite number, bigint, string, ArrayBuffer, or "
        "TypedArray"));
  }

  /**
   * null | number | bigint | string | ArrayBuffer.
   * int64: safe-integer range → Number; otherwise BigInt.
   */
  static qjs::OwnedValue to_js(Env env, const storage::SqlValue& v) {
    return std::visit(
        [&](const auto& arm) -> qjs::OwnedValue {
          using T = std::remove_cvref_t<decltype(arm)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return env.null_value();
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return store_detail::sql_int64_to_js(env, arm);
          } else {
            return Converter<T>::to_js(env, arm);
          }
        },
        v);
  }

  static qjs::OwnedValue to_js(Env env, storage::SqlValue&& v) {
    return std::visit(
        [&](auto& arm) -> qjs::OwnedValue {
          using T = std::remove_cvref_t<decltype(arm)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return env.null_value();
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return store_detail::sql_int64_to_js(env, arm);
          } else {
            return Converter<T>::to_js(env, std::move(arm));
          }
        },
        v);
  }
};

// Row helpers need Converter<storage::SqlValue>::to_js complete first.
namespace store_detail {

/** One query row → plain object { [column]: SqlValue }. */
[[nodiscard]] inline qjs::OwnedValue row_to_js(
    Env env,
    const std::vector<std::string>& columns,
    const std::vector<storage::SqlValue>& row) {
  qjs::OwnedValue obj = env.new_object();
  if (obj.is_exception()) {
    return obj;
  }
  for (std::size_t c = 0; c < columns.size(); ++c) {
    qjs::OwnedValue cell = Converter<storage::SqlValue>::to_js(env, row[c]);
    if (cell.is_exception()) {
      return cell;
    }
    qjs::OwnedValue set =
        set_own_property(env, obj.get(), columns[c].c_str(), std::move(cell));
    if (set.is_exception()) {
      return set;
    }
  }
  return obj;
}

template <class RowRange>
[[nodiscard]] inline qjs::OwnedValue rows_to_js_array(
    Env env,
    const std::vector<std::string>& columns,
    const RowRange& rows) {
  const std::size_t n = rows.size();
  if (!fits_uint32(n)) {
    return throw_encode_range(
        env, "query result length exceeds UINT32_MAX");
  }
  qjs::OwnedValue arr = env.new_array();
  if (arr.is_exception()) {
    return arr;
  }
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
    qjs::OwnedValue row_value = row_to_js(env, columns, rows[i]);
    if (row_value.is_exception()) {
      return row_value;
    }
    qjs::OwnedValue set =
        set_own_index(env, arr.get(), i, std::move(row_value));
    if (set.is_exception()) {
      return set;
    }
  }
  return arr;
}

}  // namespace store_detail

// ── params: vector<SqlValue> ───────────────────────────────────────────

template <>
struct Converter<std::vector<storage::SqlValue>> {
  /**
   * Whole argument undefined/null → empty params.
   * Undefined *elements* remain invalid (SqlValue rejects them).
   */
  static Result<std::vector<storage::SqlValue>> from_js(
      Env env,
      JSValueConst v) {
    if (store_detail::is_nullish(v)) {
      return std::vector<storage::SqlValue>{};
    }
    return store_detail::vector_from_js_array<storage::SqlValue>(
        env, v, "params");
  }
};

// ── OpenOptions ────────────────────────────────────────────────────────

template <>
struct Converter<storage::OpenOptions> {
  /** undefined/null → defaults (mode absent → ReadWriteCreate at open). */
  static Result<storage::OpenOptions> from_js(Env env, JSValueConst v) {
    if (store_detail::is_nullish(v)) {
      return storage::OpenOptions{};
    }
    auto obj_ok = store_detail::require_plain_object(env, v, "OpenOptions");
    if (!obj_ok) {
      return std::unexpected(std::move(obj_ok.error()));
    }
    storage::OpenOptions out{};
    auto mode_v = store_detail::get_prop(env, v, "mode");
    if (!mode_v) {
      return std::unexpected(std::move(mode_v.error()));
    }
    if (!mode_v->is_nullish()) {
      auto mode = store_detail::open_mode_from_js(env, mode_v->get());
      if (!mode) {
        return std::unexpected(std::move(mode.error()));
      }
      out.mode = *mode;
    }
    return out;
  }
};

// ── QueryOptions (js::store_module) ────────────────────────────────────

template <>
struct Converter<vacps::js::store_module::QueryOptions> {
  /** undefined/null → Database defaults (max_rows=10_000, max_bytes=nullopt). */
  static Result<vacps::js::store_module::QueryOptions> from_js(
      Env env,
      JSValueConst v) {
    if (store_detail::is_nullish(v)) {
      return vacps::js::store_module::QueryOptions{};
    }
    auto obj_ok = store_detail::require_plain_object(env, v, "QueryOptions");
    if (!obj_ok) {
      return std::unexpected(std::move(obj_ok.error()));
    }
    vacps::js::store_module::QueryOptions out{};
    auto rows_v = store_detail::get_prop(env, v, "maxRows");
    if (!rows_v) {
      return std::unexpected(std::move(rows_v.error()));
    }
    if (!rows_v->is_nullish()) {
      auto n = store_detail::nonnegative_size_from_js(
          env, rows_v->get(), "QueryOptions.maxRows");
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      out.max_rows = *n;
    }
    auto bytes_v = store_detail::get_prop(env, v, "maxBytes");
    if (!bytes_v) {
      return std::unexpected(std::move(bytes_v.error()));
    }
    if (!bytes_v->is_nullish()) {
      auto n = store_detail::nonnegative_size_from_js(
          env, bytes_v->get(), "QueryOptions.maxBytes");
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      out.max_bytes = *n;
    }
    return out;
  }
};

// ── ExpectedChanges ────────────────────────────────────────────────────

template <>
struct Converter<storage::ExpectedChanges> {
  /**
   * Exactly one of exactly / atLeast / atMost; value nonnegative safe int.
   * Missing keys may be undefined/null; supplying none or more than one fails.
   */
  static Result<storage::ExpectedChanges> from_js(Env env, JSValueConst v) {
    auto obj_ok =
        store_detail::require_plain_object(env, v, "ExpectedChanges");
    if (!obj_ok) {
      return std::unexpected(std::move(obj_ok.error()));
    }

    struct Field {
      const char* name;
      storage::ExpectedChanges::Kind kind;
    };
    static constexpr Field k_fields[] = {
        {"exactly", storage::ExpectedChanges::Kind::Exactly},
        {"atLeast", storage::ExpectedChanges::Kind::AtLeast},
        {"atMost", storage::ExpectedChanges::Kind::AtMost},
    };

    std::optional<storage::ExpectedChanges> chosen;
    for (const Field& f : k_fields) {
      auto pv = store_detail::get_prop(env, v, f.name);
      if (!pv) {
        return std::unexpected(std::move(pv.error()));
      }
      if (pv->is_nullish()) {
        continue;
      }
      auto n = Converter<std::int64_t>::from_js(env, pv->get());
      if (!n) {
        if (n.error().kind == ErrorKind::type) {
          return std::unexpected(Error::type(
              std::string{"ExpectedChanges."} + f.name +
              " must be a number"));
        }
        return std::unexpected(Error::range(
            std::string{"ExpectedChanges."} + f.name +
            " must be a nonnegative safe integer"));
      }
      if (*n < 0) {
        return std::unexpected(Error::range(
            std::string{"ExpectedChanges."} + f.name +
            " must be nonnegative"));
      }
      if (chosen.has_value()) {
        return std::unexpected(Error::type(
            "ExpectedChanges must supply exactly one of exactly, atLeast, "
            "atMost"));
      }
      chosen = storage::ExpectedChanges{f.kind, *n};
    }
    if (!chosen.has_value()) {
      return std::unexpected(Error::type(
          "ExpectedChanges must supply exactly one of exactly, atLeast, "
          "atMost"));
    }
    return *chosen;
  }
};

// ── TransactionStep ────────────────────────────────────────────────────

template <>
struct Converter<storage::TransactionStep> {
  /**
   * Object with required string sql.
   * type omitted → run; else exactly "run" | "query".
   * optional params.
   * expectedChanges only on run; maxRows/maxBytes only on query.
   * Cross-field misuse is rejected at decode time.
   */
  static Result<storage::TransactionStep> from_js(Env env, JSValueConst v) {
    auto obj_ok =
        store_detail::require_plain_object(env, v, "TransactionStep");
    if (!obj_ok) {
      return std::unexpected(std::move(obj_ok.error()));
    }

    storage::TransactionStep out{};

    // sql (required string)
    auto sql_v = store_detail::get_prop(env, v, "sql");
    if (!sql_v) {
      return std::unexpected(std::move(sql_v.error()));
    }
    if (sql_v->is_nullish()) {
      return std::unexpected(
          Error::type("TransactionStep.sql is required"));
    }
    auto sql = Converter<std::string>::from_js(env, sql_v->get());
    if (!sql) {
      return std::unexpected(
          Error::type("TransactionStep.sql must be a string"));
    }
    out.sql = std::move(*sql);

    // type?: 'run' | 'query'  (omit → run)
    auto type_v = store_detail::get_prop(env, v, "type");
    if (!type_v) {
      return std::unexpected(std::move(type_v.error()));
    }
    if (!type_v->is_nullish()) {
      auto ts = Converter<std::string>::from_js(env, type_v->get());
      if (!ts) {
        return std::unexpected(
            Error::type("TransactionStep.type must be 'run' or 'query'"));
      }
      if (*ts == "run") {
        out.type = storage::StepType::Run;
      } else if (*ts == "query") {
        out.type = storage::StepType::Query;
      } else {
        return std::unexpected(
            Error::range("TransactionStep.type must be 'run' or 'query'"));
      }
    }

    // params?: SqlParam[]  (nullish → empty via vector converter)
    auto params_v = store_detail::get_prop(env, v, "params");
    if (!params_v) {
      return std::unexpected(std::move(params_v.error()));
    }
    if (!params_v->is_nullish()) {
      auto params = Converter<std::vector<storage::SqlValue>>::from_js(
          env, params_v->get());
      if (!params) {
        return std::unexpected(std::move(params.error()));
      }
      out.params = std::move(*params);
    }

    // expectedChanges? (run only)
    auto exp_v = store_detail::get_prop(env, v, "expectedChanges");
    if (!exp_v) {
      return std::unexpected(std::move(exp_v.error()));
    }
    const bool has_expected = !exp_v->is_nullish();
    if (has_expected) {
      auto exp =
          Converter<storage::ExpectedChanges>::from_js(env, exp_v->get());
      if (!exp) {
        return std::unexpected(std::move(exp.error()));
      }
      out.expected_changes = std::move(*exp);
    }

    // maxRows? / maxBytes? (query only)
    bool has_max_rows = false;
    bool has_max_bytes = false;
    auto rows_v = store_detail::get_prop(env, v, "maxRows");
    if (!rows_v) {
      return std::unexpected(std::move(rows_v.error()));
    }
    if (!rows_v->is_nullish()) {
      has_max_rows = true;
      auto n = store_detail::nonnegative_size_from_js(
          env, rows_v->get(), "TransactionStep.maxRows");
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      out.max_rows = *n;
    }
    auto bytes_v = store_detail::get_prop(env, v, "maxBytes");
    if (!bytes_v) {
      return std::unexpected(std::move(bytes_v.error()));
    }
    if (!bytes_v->is_nullish()) {
      has_max_bytes = true;
      auto n = store_detail::nonnegative_size_from_js(
          env, bytes_v->get(), "TransactionStep.maxBytes");
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      out.max_bytes = *n;
    }

    // Cross-field combinations — reject synchronously at decode.
    if (out.type == storage::StepType::Query) {
      if (has_expected) {
        return std::unexpected(Error::type(
            "expectedChanges is only valid for run steps, not query"));
      }
    } else {
      if (has_max_rows || has_max_bytes) {
        return std::unexpected(Error::type(
            "maxRows/maxBytes are only valid for query steps, not run"));
      }
    }

    return out;
  }
};

template <>
struct Converter<std::vector<storage::TransactionStep>> {
  /** Non-empty TransactionStep[]; empty array is a synchronous decode error. */
  static Result<std::vector<storage::TransactionStep>> from_js(
      Env env,
      JSValueConst v) {
    auto steps = store_detail::vector_from_js_array<storage::TransactionStep>(
        env, v, "steps");
    if (!steps) {
      return std::unexpected(std::move(steps.error()));
    }
    if (steps->empty()) {
      return std::unexpected(
          Error::type("transaction steps must not be empty"));
    }
    return steps;
  }
};

// ── RunResult ──────────────────────────────────────────────────────────

template <>
struct Converter<storage::RunResult> {
  static qjs::OwnedValue to_js(Env env, const storage::RunResult& r) {
    qjs::OwnedValue obj = env.new_object();
    if (obj.is_exception()) {
      return obj;
    }

    qjs::OwnedValue changes =
        store_detail::sql_int64_to_js(env, r.changes);
    if (changes.is_exception()) {
      return changes;
    }
    qjs::OwnedValue set_changes = store_detail::set_own_property(
        env, obj.get(), "changes", std::move(changes));
    if (set_changes.is_exception()) {
      return set_changes;
    }

    qjs::OwnedValue rowid =
        store_detail::sql_int64_to_js(env, r.last_insert_rowid);
    if (rowid.is_exception()) {
      return rowid;
    }
    qjs::OwnedValue set_rowid = store_detail::set_own_property(
        env, obj.get(), "lastInsertRowid", std::move(rowid));
    if (set_rowid.is_exception()) {
      return set_rowid;
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, storage::RunResult&& r) {
    return to_js(env, static_cast<const storage::RunResult&>(r));
  }
};

// ── QueryResult → Row[] ────────────────────────────────────────────────

template <>
struct Converter<storage::QueryResult> {
  /** Encodes as Row[] (array of column-name → SqlValue objects). */
  static qjs::OwnedValue to_js(Env env, const storage::QueryResult& qr) {
    return store_detail::rows_to_js_array(env, qr.columns, qr.rows);
  }

  static qjs::OwnedValue to_js(Env env, storage::QueryResult&& qr) {
    return to_js(env, static_cast<const storage::QueryResult&>(qr));
  }
};

// ── TransactionResult ──────────────────────────────────────────────────

template <>
struct Converter<storage::TransactionResult> {
  /** RunResult object, or Row[] for query steps. */
  static qjs::OwnedValue to_js(
      Env env,
      const storage::TransactionResult& tr) {
    return std::visit(
        [&](const auto& arm) -> qjs::OwnedValue {
          using T = std::remove_cvref_t<decltype(arm)>;
          return Converter<T>::to_js(env, arm);
        },
        tr);
  }

  static qjs::OwnedValue to_js(Env env, storage::TransactionResult&& tr) {
    return std::visit(
        [&](auto& arm) -> qjs::OwnedValue {
          using T = std::remove_cvref_t<decltype(arm)>;
          return Converter<T>::to_js(env, std::move(arm));
        },
        tr);
  }
};

// ── vector<TransactionResult> ──────────────────────────────────────────

template <>
struct Converter<std::vector<storage::TransactionResult>> {
  static qjs::OwnedValue to_js(
      Env env,
      const std::vector<storage::TransactionResult>& results) {
    if (!store_detail::fits_uint32(results.size())) {
      return store_detail::throw_encode_range(
          env, "transaction result length exceeds UINT32_MAX");
    }
    qjs::OwnedValue arr = env.new_array();
    if (arr.is_exception()) {
      return arr;
    }
    const auto n = static_cast<std::uint32_t>(results.size());
    for (std::uint32_t i = 0; i < n; ++i) {
      qjs::OwnedValue step =
          Converter<storage::TransactionResult>::to_js(env, results[i]);
      if (step.is_exception()) {
        return step;
      }
      qjs::OwnedValue set =
          store_detail::set_own_index(env, arr.get(), i, std::move(step));
      if (set.is_exception()) {
        return set;
      }
    }
    return arr;
  }

  static qjs::OwnedValue to_js(
      Env env,
      std::vector<storage::TransactionResult>&& results) {
    if (!store_detail::fits_uint32(results.size())) {
      return store_detail::throw_encode_range(
          env, "transaction result length exceeds UINT32_MAX");
    }
    qjs::OwnedValue arr = env.new_array();
    if (arr.is_exception()) {
      return arr;
    }
    const auto n = static_cast<std::uint32_t>(results.size());
    for (std::uint32_t i = 0; i < n; ++i) {
      qjs::OwnedValue step = Converter<storage::TransactionResult>::to_js(
          env, std::move(results[i]));
      if (step.is_exception()) {
        return step;
      }
      qjs::OwnedValue set =
          store_detail::set_own_index(env, arr.get(), i, std::move(step));
      if (set.is_exception()) {
        return set;
      }
    }
    return arr;
  }
};

}  // namespace vacps::binding
