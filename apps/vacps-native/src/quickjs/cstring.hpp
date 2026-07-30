#pragma once

#include <quickjs.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::js {

/** RAII for JS_ToCString / JS_FreeCString. */
class CString {
 public:
  CString() noexcept = default;

  CString(JSContext* ctx, const char* ptr, std::size_t len) noexcept
      : ctx_(ctx), ptr_(ptr), len_(len) {}

  CString(const CString&) = delete;
  CString& operator=(const CString&) = delete;

  CString(CString&& other) noexcept
      : ctx_(std::exchange(other.ctx_, nullptr)),
        ptr_(std::exchange(other.ptr_, nullptr)),
        len_(std::exchange(other.len_, 0)) {}

  CString& operator=(CString&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
      ptr_ = std::exchange(other.ptr_, nullptr);
      len_ = std::exchange(other.len_, 0);
    }
    return *this;
  }

  ~CString() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr && ptr_ != nullptr) {
      JS_FreeCString(ctx_, ptr_);
    }
    ctx_ = nullptr;
    ptr_ = nullptr;
    len_ = 0;
  }

  [[nodiscard]] static CString from_value(JSContext* ctx, JSValueConst v) {
    std::size_t len = 0;
    const char* p = JS_ToCStringLen(ctx, &len, v);
    return CString{ctx, p, len};
  }

  [[nodiscard]] bool empty() const noexcept { return ptr_ == nullptr; }
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

}  // namespace vacps::js
