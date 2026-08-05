#pragma once

/**
 * Runtime::Script — owner-thread module eval / export invoke door.
 * Sibling to Runtime::Async / Runtime::Callbacks. Host-facing only; no
 * io_context, raw engine, namespace retention, promise polling, or handle
 * registry.
 *
 * Contract: Narrow
 * Preconditions:
 *   - JS owner thread, engine open, phase exactly running. These conditions
 *     are established by the caller and are not dynamically checked.
 *   - JSModuleDef* is non-owning (runtime/engine owned); null definition and
 *     null/empty export-name arguments are C++ caller misuse.
 *   - Empty module source is valid JavaScript and is not rejected merely for
 *     being empty.
 * Errors:
 *   - Result reports JS compile/eval/call failures and missing/non-callable
 *     exports discovered in the user module (operational script errors).
 *   - No recoverable Result for owner/phase/null-argument programmer misuse.
 * Threading: owner only
 * Lifetime: owning Runtime/Impl outlives every use; completion / call results
 * are caller-owned qjs::OwnedValue. Pending QuickJS exceptions are always
 * consumed into runtime::Error.
 */

#include "runtime/error.hpp"
#include "runtime/runtime_fwd.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <string_view>

namespace vacps::runtime {

struct EvaluatedModule {
  /** Non-owning; lifetime is the open engine / runtime module table. */
  JSModuleDef* definition{nullptr};
  /** Caller-owned module evaluation completion (may be a Promise). */
  vacps::qjs::OwnedValue completion;
};

}  // namespace vacps::runtime

namespace vacps {

class Runtime::Script {
 public:
  /**
   * Non-owning reference to the owning Runtime::Impl. Valid for the full Runtime
   * lifetime under the documented Narrow contract.
   */
  explicit Script(Impl& impl) noexcept;

  Script(const Script&) = delete;
  Script& operator=(const Script&) = delete;
  Script(Script&&) = delete;
  Script& operator=(Script&&) = delete;

  /**
   * Compile + evaluate a JS module source on the owner thread.
   * COMPILE_ONLY → capture JSModuleDef* → JS_EvalFunction (consumes compiled).
   * Schedules the bounded job pump after success.
   * Empty source is valid JavaScript.
   */
  [[nodiscard]] runtime::Result<runtime::EvaluatedModule> evaluate_module(
      std::string_view source,
      std::string_view filename);

  /**
   * Call a zero-argument named export on an evaluated module.
   * Namespace and export values are RAII temporaries only.
   * Schedules the bounded job pump after success.
   * Null definition / null or empty export name are Narrow misuse.
   * Missing or non-callable export is an operational script error.
   */
  [[nodiscard]] runtime::Result<vacps::qjs::OwnedValue> invoke_export(
      JSModuleDef* definition,
      const char* name);

 private:
  [[nodiscard]] JSContext* prepare_entry() const noexcept;

  Impl& impl_;
};

}  // namespace vacps
