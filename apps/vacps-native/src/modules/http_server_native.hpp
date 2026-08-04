#pragma once

/**
 * Module-private native state for vacps:http Server (not a public domain type).
 *
 * Owns the JS onRequest root and the domain http::Server (optional, emplaced
 * after construction wiring). Holds a non-owning Runtime::Callbacks& into the
 * Runtime::Impl capabilities. ClassJsEdges marks/releases only the callback edge.
 *
 * Lifetime:
 * - ServerNative itself is shared_ptr-backed (ClassBuilder async frames need it).
 * - Domain Server is stored by value in optional (not shared_ptr); Domain Server
 *   already self-retains its private State across suspension points.
 * - Transport handler captures weak_ptr<ServerNative> (no Native ↔ Server cycle).
 * - Contract: correct JS shutdown (explicit Server.close after product drain)
 *   plus Asio natural drain. There is no Runtime JS-handle cleanup-registry
 *   fallback that drops on_request before FreeContext.
 * - Explicit Server.close is business shutdown: await domain drain, then
 *   drop_on_request on the owner thread.
 * - ClassJsEdges::release is VM bookkeeping only (finalizer edge free); it is
 *   not business close and not a shutdown cleanup substitute.
 * - When those paths empty on_request first, the default destructor does not
 *   touch QuickJS. Construction-failure paths may leave a rooted callback and
 *   let OwnedValue::reset run on the owner thread when the temporary shared_ptr
 *   drops — that is intentional and owner-thread-safe.
 * - Domain ~Server only posts dispose (noexcept / non-blocking).
 */

#include "binding/detail/class_storage.hpp"
#include "http/server.hpp"
#include "qjs/owned_value.hpp"
#include "runtime/runtime.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace vacps::js::http_server {

struct ServerNative {
  vacps::qjs::OwnedValue on_request;
  /** Non-owning; Impl-owned Callbacks (valid for Runtime lifetime). */
  vacps::Runtime::Callbacks& callbacks;
  /** Emplaced once construction wiring succeeds; empty before that / never reset. */
  std::optional<vacps::http::Server> server;

  ServerNative(
      vacps::Runtime::Callbacks& callbacks_in,
      vacps::qjs::OwnedValue on_request_in) noexcept
      : on_request(std::move(on_request_in)), callbacks(callbacks_in) {}

  ServerNative(const ServerNative&) = delete;
  ServerNative& operator=(const ServerNative&) = delete;
  ServerNative(ServerNative&&) = delete;
  ServerNative& operator=(ServerNative&&) = delete;

  /**
   * noexcept. Prefer empty on_request via explicit close (business) or
   * ClassJsEdges::release (VM bookkeeping). No shutdown-cleanup fallback.
   * If a construction-failure temporary still owns a callback, ~OwnedValue may
   * FreeValue on this owner thread. Domain server drop only posts dispose.
   */
  ~ServerNative() noexcept = default;

  /** Owner-thread: drop the callback root (idempotent). Business close path. */
  void drop_on_request() noexcept {
    on_request.reset();
  }
};

}  // namespace vacps::js::http_server

namespace vacps::binding {

template <>
struct ClassJsEdges<vacps::js::http_server::ServerNative> {
  static constexpr bool enabled = true;

  static void mark(
      JSRuntime* rt,
      const vacps::js::http_server::ServerNative& self,
      JS_MarkFunc* mark_func) noexcept {
    if (!self.on_request.empty()) {
      JS_MarkValue(rt, self.on_request.get(), mark_func);
    }
  }

  /**
   * VM edge bookkeeping only: release owned callback via OwnedValue::release +
   * JS_FreeValueRT. No JS_Call, allocation, blocking, close, or business work.
   * Not a substitute for explicit Server.close or JS shutdown orchestration.
   */
  static void release(
      JSRuntime* rt,
      vacps::js::http_server::ServerNative& self) noexcept {
    if (!self.on_request.empty()) {
      JSValue v = self.on_request.release();
      JS_FreeValueRT(rt, v);
    }
  }
};

}  // namespace vacps::binding
