#pragma once

/**
 * Move-only RAII for JS_ToCStringLen / JS_FreeCString.
 *
 * Neutral QuickJS primitive (vacps::qjs). No Binding or Runtime error policy:
 * from_value leaves any pending QuickJS exception for the caller to handle.
 * Context and owner-thread validity are Narrow caller preconditions.
 */

#include <quickjs.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::qjs {

class ScopedCString {
 public:
  ScopedCString() noexcept = default;

  ScopedCString(JSContext* ctx, const char* ptr, std::size_t len) noexcept
      : ctx_(ctx), ptr_(ptr), len_(len) {}

  ScopedCString(const ScopedCString&) = delete;
  ScopedCString& operator=(const ScopedCString&) = delete;

  ScopedCString(ScopedCString&& other) noexcept
      : ctx_(std::exchange(other.ctx_, nullptr)),
        ptr_(std::exchange(other.ptr_, nullptr)),
        len_(std::exchange(other.len_, 0)) {}

  ScopedCString& operator=(ScopedCString&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
      ptr_ = std::exchange(other.ptr_, nullptr);
      len_ = std::exchange(other.len_, 0);
    }
    return *this;
  }

  ~ScopedCString() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr && ptr_ != nullptr) {
      JS_FreeCString(ctx_, ptr_);
    }
    ctx_ = nullptr;
    ptr_ = nullptr;
    len_ = 0;
  }

  /**
   * Coerce any JS value via JS_ToCStringLen.
   * On failure returns empty() and leaves the engine exception pending.
   */
  [[nodiscard]] static ScopedCString from_value(
      JSContext* ctx,
      JSValueConst v) {
    std::size_t len = 0;
    const char* p = JS_ToCStringLen(ctx, &len, v);
    return ScopedCString{ctx, p, len};
  }

  [[nodiscard]] bool empty() const noexcept { return ptr_ == nullptr; }
  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }
  [[nodiscard]] const char* c_str() const noexcept { return ptr_; }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }
  [[nodiscard]] std::string_view view() const noexcept {
    return ptr_ != nullptr ? std::string_view{ptr_, len_} : std::string_view{};
  }
  [[nodiscard]] std::string str() const { return std::string{view()}; }

 private:
  JSContext* ctx_{nullptr};
  const char* ptr_{nullptr};
  std::size_t len_{0};
};

}  // namespace vacps::qjs
