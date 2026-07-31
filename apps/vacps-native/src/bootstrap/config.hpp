#pragma once

/**
 * C++ process bootstrap only — not a product Config bag.
 *
 * Design (temp/n1.md §十):
 * - The sole bulk env read is BootstrapConfig::fromEnvironment().
 * - Typed fields are process/engine knobs that C++ must own before JS runs.
 * - Product policy (listen host/port, auth, CONTROL_PLANE_*, FS product roots, …)
 *   stays in JS via host.getenv() → EnvironmentSnapshot. Do not re-mirror
 *   loadConfig() into this struct.
 * - FS / HTTP / Bindings never call getenv(); inject from composition or snapshot.
 */

#include "bootstrap/environment.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace vacps::bootstrap {

struct BootstrapConfig {
  // ── process bootstrap (C++ owns) ───────────────────────────────
  /** VACPS_DATA_DIR; default "data". Also exposed as host.dataDir(). */
  std::string data_dir{"data"};
  /** VACPS_LOG_LEVEL; default "info". Applied before JS loads. */
  std::string log_level{"info"};
  /** VACPS_SCRIPT; empty → caller resolves default path. */
  std::string script_path;
  /**
   * VACPS_CA_BUNDLE for C++ outbound TLS (http client).
   * Empty → platform default resolution at HttpRuntime construction.
   */
  std::string ca_bundle;

  // ── QuickJS engine (C++ owns; not product policy) ──────────────
  /** VACPS_JS_HEAP_LIMIT_BYTES; default 32 MiB. */
  std::size_t js_heap_limit_bytes{32u * 1024u * 1024u};
  /** VACPS_JS_STACK_LIMIT_BYTES; default 1 MiB. */
  std::size_t js_stack_limit_bytes{1u * 1024u * 1024u};
  /** VACPS_JS_TIME_BUDGET_MS; default 30000. 0 disables interrupt watchdog. */
  std::chrono::milliseconds js_time_budget{30'000};

  /**
   * Full process environ at fromEnvironment() time.
   * host.getenv() and any remaining product knobs read only from here.
   */
  EnvironmentSnapshot environment;

  /** Capture environ once and fill C++-owned fields. Bootstrap only. */
  [[nodiscard]] static BootstrapConfig fromEnvironment();

  /** Parse C++-owned keys from an existing snapshot (no further getenv). */
  [[nodiscard]] static BootstrapConfig from_snapshot(EnvironmentSnapshot snapshot);
};

}  // namespace vacps::bootstrap
