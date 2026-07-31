#pragma once

#include "app/error.hpp"
#include "quickjs/runtime.hpp"
#include "quickjs/raii/value.hpp"

#include <quickjs.h>

#include <string_view>
#include <utility>

namespace vacps::js {

/** Owns JSContext for a Runtime (Realm). Destroy before Runtime. */
class Context {
 public:
  Context() noexcept = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  Context(Context&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

  Context& operator=(Context&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
  }

  ~Context() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr) {
      JS_FreeContext(ctx_);
      ctx_ = nullptr;
    }
  }

  [[nodiscard]] static Result<Context> create(Runtime& runtime);

  [[nodiscard]] JSContext* get() const noexcept { return ctx_; }
  [[nodiscard]] bool ok() const noexcept { return ctx_ != nullptr; }

  /** Capture current exception into Error message (and free the exception value). */
  [[nodiscard]] Error take_exception_error();

  /**
   * Evaluate source. On exception returns Error (exception cleared).
   * Does not drain promise jobs — caller (ScriptRuntime) should drain.
   */
  [[nodiscard]] Result<Value> eval(
      std::string_view source,
      std::string_view filename = "<eval>",
      int flags = JS_EVAL_TYPE_GLOBAL);

 private:
  explicit Context(JSContext* ctx) noexcept : ctx_(ctx) {}

  JSContext* ctx_{nullptr};
};

}  // namespace vacps::js
