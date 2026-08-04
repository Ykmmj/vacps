#pragma once

/**
 * CallbackInfo — read-only view of ctx / this / argc / argv for adapted
 * QuickJS callbacks.
 *
 * argv[i] and this_val are borrowed (QuickJS owns them for the call duration).
 * Do not JS_FreeValue them. Returned conversions are owned by the caller.
 */

#include "binding/convert.hpp"
#include "binding/env.hpp"
#include "binding/error.hpp"
#include "binding/value_ref.hpp"

#include <quickjs.h>

#include <string>
#include <utility>

namespace vacps::binding {

class CallbackInfo {
 public:
  CallbackInfo(
      Env env,
      JSValueConst this_val,
      int argc,
      JSValueConst* argv) noexcept
      : env_(env), this_val_(this_val), argc_(argc), argv_(argv) {}

  [[nodiscard]] Env env() const noexcept { return env_; }
  [[nodiscard]] JSContext* context() const noexcept {
    return env_.context();
  }

  [[nodiscard]] int length() const noexcept { return argc_; }
  [[nodiscard]] bool empty() const noexcept { return argc_ <= 0; }

  /** Borrowed this-value (do not free). */
  [[nodiscard]] ValueRef this_value() const noexcept {
    return ValueRef{env_, this_val_};
  }

  [[nodiscard]] JSValueConst this_raw() const noexcept { return this_val_; }

  /**
   * Argument i, or undefined when out of range (does not throw).
   * Borrowed — do not free.
   */
  [[nodiscard]] ValueRef operator[](int i) const noexcept {
    if (i < 0 || i >= argc_ || argv_ == nullptr) {
      return ValueRef{env_, JS_UNDEFINED};
    }
    return ValueRef{env_, argv_[i]};
  }

  /** Require at least `n` arguments; TypeError otherwise. */
  [[nodiscard]] VoidResult check_argc(int n, const char* what = nullptr) const {
    if (argc_ < n) {
      std::string msg = "expected at least ";
      msg += std::to_string(n);
      msg += " argument(s)";
      if (what != nullptr) {
        msg += " for ";
        msg += what;
      }
      return std::unexpected(Error::type(std::move(msg)));
    }
    return {};
  }

  /**
   * Decode argument i via Converter<T>. Missing / out-of-range indices are
   * borrowed JS_UNDEFINED (same as operator[]), matching JS call semantics.
   * Required primitive converters still reject undefined; DTO converters may
   * intentionally default it.
   */
  template <class T>
  [[nodiscard]] Result<T> arg(int i) const {
    return Converter<T>::from_js(env_, (*this)[i].get());
  }

 private:
  Env env_;
  JSValueConst this_val_;
  int argc_;
  JSValueConst* argv_;
};

}  // namespace vacps::binding
