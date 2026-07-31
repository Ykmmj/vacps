#include "url/url.hpp"

namespace vacps::url {

Url::Url(ada::url_aggregator url) noexcept : url_(std::move(url)) {}

Url::Url(const Url& other) : url_(other.url_) {
  // Fresh instance: do not share the other Url's live SearchParams (owner would
  // be wrong). Lazy-create on search_params() from current search().
}

Url& Url::operator=(const Url& other) {
  if (this != &other) {
    detach_live_params();
    url_ = other.url_;
    live_params_.reset();
  }
  return *this;
}

Url::Url(Url&& other) noexcept
    : url_(std::move(other.url_)), live_params_(std::move(other.live_params_)) {
  reattach_live_params();
}

Url& Url::operator=(Url&& other) noexcept {
  if (this != &other) {
    detach_live_params();
    url_ = std::move(other.url_);
    live_params_ = std::move(other.live_params_);
    reattach_live_params();
  }
  return *this;
}

Url::~Url() { detach_live_params(); }

void Url::detach_live_params() noexcept {
  if (live_params_) {
    live_params_->detach_owner();
  }
}

void Url::reattach_live_params() noexcept {
  if (live_params_) {
    live_params_->attach_owner(this);
  }
}

Result<Url> Url::parse(std::string_view input) {
  auto parsed = ada::parse<ada::url_aggregator>(input);
  if (!parsed) {
    return std::unexpected(Error{"Invalid URL"});
  }
  return Url{std::move(*parsed)};
}

Result<Url> Url::parse(std::string_view input, std::string_view base) {
  auto base_url = ada::parse<ada::url_aggregator>(base);
  if (!base_url) {
    return std::unexpected(Error{"Invalid base URL"});
  }
  auto parsed = ada::parse<ada::url_aggregator>(input, &*base_url);
  if (!parsed) {
    return std::unexpected(Error{"Invalid URL"});
  }
  return Url{std::move(*parsed)};
}

Result<Url> Url::parse(std::string_view input, const Url& base) {
  auto parsed = ada::parse<ada::url_aggregator>(input, &base.url_);
  if (!parsed) {
    return std::unexpected(Error{"Invalid URL"});
  }
  return Url{std::move(*parsed)};
}

bool Url::can_parse(std::string_view input) noexcept {
  return ada::can_parse(input);
}

bool Url::can_parse(std::string_view input, std::string_view base) noexcept {
  return ada::can_parse(input, &base);
}

std::string_view Url::href() const noexcept { return url_.get_href(); }

std::string_view Url::protocol() const noexcept { return url_.get_protocol(); }

std::string_view Url::host() const noexcept { return url_.get_host(); }

std::string_view Url::hostname() const noexcept { return url_.get_hostname(); }

std::string_view Url::port() const noexcept { return url_.get_port(); }

std::string_view Url::pathname() const noexcept { return url_.get_pathname(); }

std::string_view Url::search() const noexcept { return url_.get_search(); }

std::string_view Url::hash() const noexcept { return url_.get_hash(); }

void Url::set_search(std::string_view search) {
  url_.set_search(search);
  if (live_params_) {
    // Source of truth is Ada; re-parse without notify_owner feedback.
    live_params_->reset(url_.get_search());
  }
}

void Url::apply_search_from_params(std::string_view serialized) {
  // SearchParams::to_string has no leading '?'; empty clears. Ada accepts both.
  url_.set_search(serialized);
}

std::shared_ptr<SearchParams> Url::search_params() {
  if (!live_params_) {
    live_params_ = std::make_shared<SearchParams>(url_.get_search());
    live_params_->attach_owner(this);
  }
  return live_params_;
}

std::shared_ptr<SearchParams> Url::search_params_if_any() const noexcept {
  return live_params_;
}

std::string Url::origin() const { return url_.get_origin(); }

std::string_view Url::username() const noexcept { return url_.get_username(); }

std::string_view Url::password() const noexcept { return url_.get_password(); }

}  // namespace vacps::url
