#pragma once

/**
 * ModuleBuilder — declare namespace / module-object properties and functions,
 * then commit once onto a new or existing object.
 *
 * Not a JSModuleDef loader (see existing quickjs module catalog for that).
 * This builder produces plain objects suitable for:
 *   - JS_SetModuleExport(..., obj) after a prior JS_AddModuleExport
 *   - global namespaces
 *   - nested property bags
 *
 * Ownership:
 * - Staged qjs::OwnedValue properties are held by the builder until commit.
 * - commit / commit_to transfer each value with JS_SetPropertyStr, which
 *   always takes ownership (including on failure — frees the value).
 * - After release(), the builder's qjs::OwnedValue is empty → destructor is a
 *   no-op (no double-free).
 * - On failure mid-commit, already-set properties stay on the target;
 *   unset staged values are freed by ~qjs::OwnedValue.
 * - Result-returning APIs (commit, commit_to) never leave a pending JS
 *   exception: staged failures are absorbed immediately into Error.
 *
 * Staging contract:
 * - value()/function()/object()/… detect JS_EXCEPTION immediately, clear the
 *   pending exception, drop the sentinel, and record a native staging Error.
 * - Property names are non-null and qjs::OwnedValue arguments are non-empty
 *   and belong to this builder's JSContext. These are Narrow caller
 *   preconditions and are not checked by the builder.
 * - After a staging error, further staging methods do not call QuickJS.
 *
 * Module export two-phase API (QuickJS requirement):
 * - declare_export(m, name) → JS_AddModuleExport  (module creation time)
 * - export_as(m, name)      → commit + JS_SetModuleExport (module init)
 * export_as does NOT call AddModuleExport; the export must already be
 * declared. Calling Set without Add fails.
 */

#include "binding/async_function.hpp"
#include "binding/detail/async_traits.hpp"
#include "binding/env.hpp"
#include "binding/error.hpp"
#include "binding/function.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::binding {

class ModuleBuilder {
 public:
  explicit ModuleBuilder(Env ctx) noexcept : ctx_(ctx) {}

  [[nodiscard]] Env env() const noexcept { return ctx_; }

  /**
   * Stage an owned value property (moved).
   * Preconditions: name is non-null; v is non-empty and owned by this
   * builder's JSContext.
   */
  ModuleBuilder& value(const char* name, qjs::OwnedValue v) {
    const std::string entry_name{name};
    (void)absorb_staged(entry_name, std::move(v));
    return *this;
  }

  /** Stage a duplicated value from a borrowed JSValueConst. */
  ModuleBuilder& value_dup(const char* name, JSValueConst v) {
    if (staging_error_) {
      return *this;
    }
    return value(
        name,
        qjs::OwnedValue{ctx_.context(), JS_DupValue(ctx_.context(), v)});
  }

  ModuleBuilder& string(const char* name, std::string_view s) {
    if (staging_error_) {
      return *this;
    }
    return value(name, ctx_.string(s));
  }

  ModuleBuilder& boolean(const char* name, bool b) {
    if (staging_error_) {
      return *this;
    }
    return value(name, ctx_.boolean(b));
  }

  ModuleBuilder& int32(const char* name, std::int32_t n) {
    if (staging_error_) {
      return *this;
    }
    return value(name, ctx_.int32(n));
  }

  ModuleBuilder& float64(const char* name, double n) {
    if (staging_error_) {
      return *this;
    }
    return value(name, ctx_.float64(n));
  }

  /** Stage a free function / lambda as a property (captures ctx.async()). */
  template <class Fn>
  ModuleBuilder& function(const char* name, Fn fn, int length = 0) {
    if (staging_error_) {
      return *this;
    }
    return value(name, create_function(ctx_, name, std::move(fn), length));
  }

  /**
   * Stage an async free function / lambda as a Promise-returning property.
   * Same staging/ownership semantics as function(); a usable
   * vacps::Runtime::Async* is a Narrow Env composition precondition.
   *
   * @param length JS function.length. Default -1 defers to
   *        create_async_function's decoded-arity default. Pass 0 (or any
   *        non-negative value) for an explicit override.
   */
  template <class Fn>
  ModuleBuilder& async_function(const char* name, Fn fn, int length = -1) {
    if (staging_error_) {
      return *this;
    }
    return value(
        name, create_async_function(ctx_, name, std::move(fn), length));
  }

  /**
   * async_function with a custom owner-thread Encode (runtime::JsEncode).
   * Encode is distinguished from length by not being convertible to int.
   * Not available for Task<void> (always resolves undefined).
   */
  template <class Fn, class Encode>
    requires detail::is_async_encode_arg_v<Encode>
  ModuleBuilder& async_function(
      const char* name,
      Fn fn,
      Encode encode,
      int length = -1) {
    if (staging_error_) {
      return *this;
    }
    return value(
        name,
        create_async_function(
            ctx_, name, std::move(fn), std::move(encode), length));
  }

  /**
   * Nested object: invoke `fn(child_builder)`, commit child, stage as property.
   * Child commit failures become a staging Error (engine clean). Once a
   * staging error exists, the nested callback is not invoked.
   */
  template <class Fn>
  ModuleBuilder& object(const char* name, Fn fn) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    ModuleBuilder child{ctx_};
    fn(child);
    auto nested = child.commit();
    if (!nested) {
      staging_error_ = std::move(nested.error());
      if (staging_error_->message.empty()) {
        staging_error_ = Error::internal(
            "ModuleBuilder: nested object failed for " +
            (entry_name.empty() ? std::string{"<unnamed>"} : entry_name));
      }
      return *this;
    }
    (void)absorb_staged(entry_name, std::move(*nested));
    return *this;
  }

  /**
   * Create a new plain object and set all staged properties.
   * @return Caller-owned object, or unexpected Error (engine clean).
   */
  [[nodiscard]] Result<qjs::OwnedValue> commit() {
    if (staging_error_) {
      entries_.clear();
      return std::unexpected(std::move(*staging_error_));
    }
    qjs::OwnedValue obj = ctx_.new_object();
    if (obj.is_exception()) {
      clear_exception(ctx_.context());
      (void)obj.release();
      return std::unexpected(
          Error::internal("ModuleBuilder: NewObject failed"));
    }
    auto r = commit_to(obj.get());
    if (!r) {
      return std::unexpected(std::move(r.error()));
    }
    return obj;
  }

  /**
   * Set all staged properties onto an existing object.
   * Each JS_SetPropertyStr consumes the staged value exactly once.
   * Result contract: no pending JS exception on return.
   */
  [[nodiscard]] VoidResult commit_to(JSValueConst target) {
    if (staging_error_) {
      entries_.clear();
      return std::unexpected(std::move(*staging_error_));
    }
    JSContext* c = ctx_.context();
    for (auto& e : entries_) {
      JSValue raw = e.value.release();
      // Ownership of raw transfers here. On failure QuickJS frees raw.
      if (JS_SetPropertyStr(c, target, e.name.c_str(), raw) < 0) {
        clear_exception(c);
        entries_.clear();
        return std::unexpected(Error::internal(
            "ModuleBuilder: SetProperty failed: " + e.name));
      }
    }
    entries_.clear();
    return {};
  }

  /**
   * Phase 1 of module export: declare an export name on the module def.
   * Must be called at module creation time (alongside other AddModuleExport).
   * @return 0 on success, -1 on failure.
   */
  static int declare_export(
      JSContext* ctx,
      JSModuleDef* m,
      const char* export_name) {
    return JS_AddModuleExport(ctx, m, export_name);
  }

  [[nodiscard]] int declare_export(JSModuleDef* m, const char* export_name) {
    return declare_export(ctx_.context(), m, export_name);
  }

  /**
   * Phase 2 of module export: commit a new object and JS_SetModuleExport.
   *
   * REQUIRES a prior declare_export / JS_AddModuleExport for `export_name`.
   * This does not call AddModuleExport (wrong phase).
   *
   * @return 0 on success, -1 on failure. On binding-level failure a JS
   * exception is thrown (init-function style). On SetModuleExport failure the
   * engine's pending exception is preserved. The committed value is released
   * into SetModuleExport (engine owns it); on Set failure QuickJS frees it.
   */
  [[nodiscard]] int export_as(JSModuleDef* m, const char* export_name) {
    auto obj = commit();
    if (!obj) {
      (void)throw_error(ctx_.context(), obj.error());
      return -1;
    }
    // JS_SetModuleExport takes ownership of the value.
    JSValue raw = obj->release();
    if (JS_SetModuleExport(ctx_.context(), m, export_name, raw) < 0) {
      // Engine owns/frees raw; pending exception preserved.
      return -1;
    }
    return 0;
  }

  /**
   * Set an already-built owned value as a module export (no commit).
   * Same declare_export precondition as export_as.
   * `value` must be non-empty and belong to this builder's JSContext.
   */
  [[nodiscard]] int set_export(
      JSModuleDef* m,
      const char* export_name,
      qjs::OwnedValue value) {
    JSContext* c = ctx_.context();
    if (value.is_exception()) {
      (void)value.release();
      return -1;
    }
    JSValue raw = value.release();
    if (JS_SetModuleExport(c, m, export_name, raw) < 0) {
      return -1;
    }
    return 0;
  }

 private:
  struct Entry {
    std::string name;
    qjs::OwnedValue value;
  };

  /**
   * Absorb a value for staging. Clears a QuickJS creation exception and
   * records the first staging Error. Returns true if staged.
   */
  bool absorb_staged(std::string name, qjs::OwnedValue v) {
    if (staging_error_) {
      detail::discard_owned(std::move(v));
      return false;
    }
    if (v.is_exception()) {
      clear_exception(ctx_.context());
      (void)v.release();
      staging_error_ = Error::internal(
          "ModuleBuilder: staged exception for " +
          (name.empty() ? std::string{"<unnamed>"} : name));
      return false;
    }
    entries_.push_back(Entry{std::move(name), std::move(v)});
    return true;
  }

  Env ctx_;
  std::vector<Entry> entries_;
  std::optional<Error> staging_error_;
};

}  // namespace vacps::binding
