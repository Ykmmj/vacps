#include "url/search_params.hpp"

#include "url/url.hpp"

namespace vacps::url {

SearchParams::SearchParams(std::string_view init) : params_(init) {}

SearchParams::SearchParams(const SearchParams& other)
    : params_(other.params_), owner_(nullptr) {}

SearchParams& SearchParams::operator=(const SearchParams& other) {
  if (this != &other) {
    params_ = other.params_;
    // Keep current owner_ — assignment replaces pairs only. Callers that need
    // a detached snapshot copy-construct instead.
  }
  return *this;
}

SearchParams::SearchParams(SearchParams&& other) noexcept
    : params_(std::move(other.params_)), owner_(nullptr) {
  // Do not steal owner_: Url's shared_ptr still names `other` or is empty.
  other.owner_ = nullptr;
}

SearchParams& SearchParams::operator=(SearchParams&& other) noexcept {
  if (this != &other) {
    params_ = std::move(other.params_);
    other.owner_ = nullptr;
  }
  return *this;
}

SearchParams::~SearchParams() {
  owner_ = nullptr;
}

void SearchParams::attach_owner(Url* owner) noexcept { owner_ = owner; }

void SearchParams::detach_owner() noexcept { owner_ = nullptr; }

void SearchParams::notify_owner() {
  if (owner_ != nullptr) {
    owner_->apply_search_from_params(to_string());
  }
}

void SearchParams::reset(std::string_view init) {
  params_.reset(init);
}

void SearchParams::append(std::string_view key, std::string_view value) {
  params_.append(key, value);
  notify_owner();
}

void SearchParams::set(std::string_view key, std::string_view value) {
  params_.set(key, value);
  notify_owner();
}

std::optional<std::string> SearchParams::get(std::string_view key) const {
  auto v = params_.get(key);
  if (!v) {
    return std::nullopt;
  }
  return std::string{*v};
}

std::vector<std::string> SearchParams::get_all(std::string_view key) const {
  return params_.get_all(key);
}

bool SearchParams::has(std::string_view key) const noexcept {
  return params_.has(key);
}

bool SearchParams::has(std::string_view key, std::string_view value) const noexcept {
  return params_.has(key, value);
}

void SearchParams::remove(std::string_view key) {
  params_.remove(key);
  notify_owner();
}

void SearchParams::remove(std::string_view key, std::string_view value) {
  params_.remove(key, value);
  notify_owner();
}

void SearchParams::sort() {
  params_.sort();
  notify_owner();
}

std::string SearchParams::to_string() const { return params_.to_string(); }

std::size_t SearchParams::size() const noexcept { return params_.size(); }

}  // namespace vacps::url
