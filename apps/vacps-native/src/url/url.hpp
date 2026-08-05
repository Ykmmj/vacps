#pragma once

#include "app/error.hpp"
#include "url/search_params.hpp"

#include <ada.h>

#include <memory>
#include <string>
#include <string_view>

namespace vacps::url {

/**
 * WHATWG URL domain object (Ada `url_aggregator`).
 *
 * Pure C++ — no QuickJS. Bindings construct one per `new URL(...)` and
 * expose getters/setters; all mutations go through Ada (or SearchParams →
 * set_search), never hand-rolled string concat.
 *
 * string_view getters are views into the internal buffer and are
 * invalidated by any mutating operation on this Url.
 *
 * Live `searchParams`: `search_params()` returns a shared SearchParams bag
 * attached to this Url. Mutations on that bag call back into `set_search`.
 * Assigning search (Ada) re-parses into the live bag when it exists.
 */
class Url final {
 public:
  Url() = delete;

  Url(const Url& other);
  Url& operator=(const Url& other);
  Url(Url&& other) noexcept;
  Url& operator=(Url&& other) noexcept;
  ~Url();

  /** Parse an absolute URL. */
  [[nodiscard]] static Result<Url> parse(std::string_view input);

  /** Parse `input` relative to absolute `base`. */
  [[nodiscard]] static Result<Url> parse(
      std::string_view input,
      std::string_view base);

  /** Parse `input` relative to an already-parsed base URL. */
  [[nodiscard]] static Result<Url> parse(
      std::string_view input,
      const Url& base);

  [[nodiscard]] static bool can_parse(std::string_view input) noexcept;
  [[nodiscard]] static bool can_parse(
      std::string_view input,
      std::string_view base) noexcept;

  [[nodiscard]] std::string_view href() const noexcept;
  [[nodiscard]] std::string_view protocol() const noexcept;
  [[nodiscard]] std::string_view host() const noexcept;
  [[nodiscard]] std::string_view hostname() const noexcept;
  [[nodiscard]] std::string_view port() const noexcept;
  [[nodiscard]] std::string_view pathname() const noexcept;
  [[nodiscard]] std::string_view search() const noexcept;
  [[nodiscard]] std::string_view hash() const noexcept;

  /**
   * Set the query string (with or without leading '?').
   * Empty clears search. Updates Ada, then re-parses the live SearchParams
   * bag if one exists (no mutation notify feedback).
   * Bindings must not call raw().set_search.
   */
  void set_search(std::string_view search);

  /**
   * Live URLSearchParams view for this URL (created on first call).
   * Same shared instance for the lifetime of this Url until move/assign.
   */
  [[nodiscard]] std::shared_ptr<SearchParams> search_params();

  /** Non-null only after `search_params()` has been called on this instance. */
  [[nodiscard]] std::shared_ptr<SearchParams> search_params_if_any() const noexcept;

  /** Serialized origin (Ada allocates). */
  [[nodiscard]] std::string origin() const;

  /** Ada username component (empty if none). JS getter only — no domain setter yet. */
  [[nodiscard]] std::string_view username() const noexcept;
  /** Ada password component (empty if none). JS getter only — no domain setter yet. */
  [[nodiscard]] std::string_view password() const noexcept;

  [[nodiscard]] ada::url_aggregator& raw() noexcept { return url_; }
  [[nodiscard]] const ada::url_aggregator& raw() const noexcept { return url_; }

 private:
  friend class SearchParams;

  explicit Url(ada::url_aggregator url) noexcept;

  /** SearchParams mutation path: write serialized query into Ada only. */
  void apply_search_from_params(std::string_view serialized);

  void detach_live_params() noexcept;
  void reattach_live_params() noexcept;

  ada::url_aggregator url_;
  /** Lazy live query bag; shared so JS handles can outlive interim refs. */
  mutable std::shared_ptr<SearchParams> live_params_;
};

}  // namespace vacps::url
