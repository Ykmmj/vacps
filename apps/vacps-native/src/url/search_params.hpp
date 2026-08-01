#pragma once

#include <ada.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::url {

class Url;

/**
 * WHATWG URLSearchParams domain object (Ada `url_search_params`).
 *
 * Standalone query bag: append / set / get / getAll / has / remove / toString
 * plus index access for JS entries/keys/values/forEach iterators.
 *
 * Live view: when attached to a `Url` (via `Url::search_params()`), every
 * mutation re-serializes into the owning URL with `Url::set_search` semantics
 * (Ada only — no hand-rolled href edits). `reset` / owner attach is used by
 * `Url` so `url.search = ...` re-parses into this bag without feedback loops.
 */
class SearchParams final {
 public:
  SearchParams() = default;

  /**
   * Parse a query string (with or without leading '?').
   * Empty / oversize inputs leave the object empty (Ada behavior).
   */
  explicit SearchParams(std::string_view init);

  /** Copy is always a detached snapshot (no owner). */
  SearchParams(const SearchParams& other);
  SearchParams& operator=(const SearchParams& other);

  SearchParams(SearchParams&& other) noexcept;
  SearchParams& operator=(SearchParams&& other) noexcept;

  ~SearchParams();

  void append(std::string_view key, std::string_view value);
  void set(std::string_view key, std::string_view value);

  /** First value for `key`, or nullopt. Owned string (safe after mutation). */
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

  [[nodiscard]] std::vector<std::string> get_all(std::string_view key) const;

  [[nodiscard]] bool has(std::string_view key) const noexcept;
  [[nodiscard]] bool has(std::string_view key, std::string_view value) const noexcept;

  /** JS `delete(name)` — C++ keyword, so named `remove`. */
  void remove(std::string_view key);
  void remove(std::string_view key, std::string_view value);

  void sort();

  /**
   * Replace all pairs from a query string without notifying the owner.
   * Used when `Url::set_search` (or Ada search) is the source of truth.
   */
  void reset(std::string_view init);

  /** application/x-www-form-urlencoded body (no leading '?'). */
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] std::size_t size() const noexcept;

  /**
   * Key/value pair at `index` in list order (owned strings), or nullopt.
   * Used by JS entries/keys/values iterators and forEach.
   */
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> at(
      std::size_t index) const;

  [[nodiscard]] Url* owner() const noexcept { return owner_; }

  [[nodiscard]] ada::url_search_params& raw() noexcept { return params_; }
  [[nodiscard]] const ada::url_search_params& raw() const noexcept {
    return params_;
  }

 private:
  friend class Url;

  void attach_owner(Url* owner) noexcept;
  void detach_owner() noexcept;
  void notify_owner();

  // Ada's get/has are non-const; store mutable for const method wrappers.
  mutable ada::url_search_params params_;
  Url* owner_ = nullptr;
};

}  // namespace vacps::url
