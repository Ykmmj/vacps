#pragma once

#include <quickjs.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace vacps::js {

/** Move-only JSAtom owner. */
class Atom {
 public:
  Atom() noexcept = default;

  Atom(JSContext* ctx, JSAtom atom) noexcept : ctx_(ctx), atom_(atom) {}

  Atom(const Atom&) = delete;
  Atom& operator=(const Atom&) = delete;

  Atom(Atom&& other) noexcept
      : ctx_(std::exchange(other.ctx_, nullptr)),
        atom_(std::exchange(other.atom_, JS_ATOM_NULL)) {}

  Atom& operator=(Atom&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
      atom_ = std::exchange(other.atom_, JS_ATOM_NULL);
    }
    return *this;
  }

  ~Atom() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr && atom_ != JS_ATOM_NULL) {
      JS_FreeAtom(ctx_, atom_);
    }
    ctx_ = nullptr;
    atom_ = JS_ATOM_NULL;
  }

  [[nodiscard]] static Atom from_string(JSContext* ctx, std::string_view s) {
    return Atom{ctx, JS_NewAtomLen(ctx, s.data(), s.size())};
  }

  /** Take ownership of an atom already owned by the caller. */
  [[nodiscard]] static Atom adopt(JSContext* ctx, JSAtom atom) {
    return Atom{ctx, atom};
  }

  [[nodiscard]] JSAtom get() const noexcept { return atom_; }
  [[nodiscard]] bool empty() const noexcept {
    return ctx_ == nullptr || atom_ == JS_ATOM_NULL;
  }

  /** Release ownership without FreeAtom (e.g. for table that frees itself). */
  [[nodiscard]] JSAtom release() noexcept {
    ctx_ = nullptr;
    return std::exchange(atom_, JS_ATOM_NULL);
  }

 private:
  JSContext* ctx_{nullptr};
  JSAtom atom_{JS_ATOM_NULL};
};

/**
 * Owns JS_GetOwnPropertyNames result: frees each atom then js_free(tab).
 */
class PropertyEnumList {
 public:
  PropertyEnumList() noexcept = default;
  PropertyEnumList(JSContext* ctx, JSPropertyEnum* tab, std::uint32_t len) noexcept
      : ctx_(ctx), tab_(tab), len_(len) {}

  PropertyEnumList(const PropertyEnumList&) = delete;
  PropertyEnumList& operator=(const PropertyEnumList&) = delete;

  PropertyEnumList(PropertyEnumList&& o) noexcept
      : ctx_(std::exchange(o.ctx_, nullptr)),
        tab_(std::exchange(o.tab_, nullptr)),
        len_(std::exchange(o.len_, 0)) {}

  PropertyEnumList& operator=(PropertyEnumList&& o) noexcept {
    if (this != &o) {
      reset();
      ctx_ = std::exchange(o.ctx_, nullptr);
      tab_ = std::exchange(o.tab_, nullptr);
      len_ = std::exchange(o.len_, 0);
    }
    return *this;
  }

  ~PropertyEnumList() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr && tab_ != nullptr) {
      for (std::uint32_t i = 0; i < len_; ++i) {
        JS_FreeAtom(ctx_, tab_[i].atom);
      }
      js_free(ctx_, tab_);
    }
    ctx_ = nullptr;
    tab_ = nullptr;
    len_ = 0;
  }

  [[nodiscard]] static PropertyEnumList get_own(
      JSContext* ctx,
      JSValueConst obj,
      int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) {
    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, flags) != 0 || tab == nullptr) {
      return {};
    }
    return PropertyEnumList{ctx, tab, len};
  }

  [[nodiscard]] bool empty() const noexcept { return tab_ == nullptr || len_ == 0; }
  [[nodiscard]] std::uint32_t size() const noexcept { return len_; }
  [[nodiscard]] JSAtom atom_at(std::uint32_t i) const noexcept { return tab_[i].atom; }
  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }

 private:
  JSContext* ctx_{nullptr};
  JSPropertyEnum* tab_{nullptr};
  std::uint32_t len_{0};
};

}  // namespace vacps::js
