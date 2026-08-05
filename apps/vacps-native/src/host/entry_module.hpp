#pragma once

/**
 * EntryModule — owner-thread ESM load / initialize / shutdown lifecycle.
 *
 * Stores only a non-owning JSModuleDef*. Never retains a
 * module namespace or any long-lived JSValue. Never frees the module definition
 * (engine/runtime owned). Safe to reset() after partial failure before
 * FreeContext.
 *
 * Owner-thread affinity is a caller precondition of Runtime operations.
 */

#include "runtime/error.hpp"
#include "runtime/runtime.hpp"

#include <boost/asio/awaitable.hpp>

#include <quickjs.h>

#include <chrono>
#include <string_view>

namespace vacps::host {

namespace asio = boost::asio;

class EntryModule {
 public:
  EntryModule() = default;

  EntryModule(const EntryModule&) = delete;
  EntryModule& operator=(const EntryModule&) = delete;

  /**
   * Evaluate module source, await completion, invoke/await `initialize`.
   *
   * Contract: Narrow
   * Preconditions: owner thread; Runtime running; no load/initialize/shutdown
   * has started; definition is empty. Violations are programmer errors.
   * Caller string_views are not retained across suspension. Marks initialized
   * only after initialize fully succeeds.
   * Errors: module evaluation, exported initialize, timeout and cancellation.
   */
  [[nodiscard]] asio::awaitable<runtime::VoidResult> load_and_initialize(
      Runtime& runtime,
      std::string_view source,
      std::string_view filename,
      std::chrono::milliseconds timeout);

  /**
   * Invoke/await exported `shutdown` only when initialization fully completed.
   *
   * Contract: Narrow
   * Preconditions: owner thread; Runtime running; initialize succeeded;
   * definition is live; shutdown has not started. Violations are programmer
   * errors. This is a single-shot host operation.
   * Errors: exported shutdown, timeout and cancellation.
   * Does not mark the operation complete before the export result resolves.
   */
  [[nodiscard]] asio::awaitable<runtime::VoidResult> shutdown(
      Runtime& runtime,
      std::chrono::milliseconds timeout);

  /** Clears the non-owning definition pointer. No QuickJS calls. */
  void reset() noexcept;

 private:
  /** Non-owning; engine module table owns the definition. */
  JSModuleDef* definition_{nullptr};
};

}  // namespace vacps::host
