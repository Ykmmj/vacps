#pragma once

/**
 * Process-scoped services for native bindings (ApplicationRuntime layer).
 *
 * Owned by ApplicationRuntime (composition root). ScriptRuntime holds a
 * shared_ptr and exposes it via services().
 *
 * Fields: data_dir, ca_bundle, environment, pool, processes, use_asio_file.
 * environment is the bootstrap EnvironmentSnapshot; host.getenv reads only from it.
 * Not product policy (listen/auth) and not QuickJS engine state.
 */

#include "bootstrap/environment.hpp"
#include "process/registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace vacps::js {

namespace asio = boost::asio;

/** Construction knobs for ScriptServices (paths + snapshot; pool size optional). */
struct ScriptServicesOptions {
  std::string data_dir{"data"};
  std::string ca_bundle;
  /** Process environ captured at BootstrapConfig::fromEnvironment(). */
  vacps::bootstrap::EnvironmentSnapshot environment;
  std::size_t pool_threads{2};
};

/**
 * Shared process services for vacps:* bindings.
 *
 * Lifetime: ApplicationRuntime creates and retains a shared_ptr; ScriptRuntime
 * receives the same pointer at create. Destructor stops the process registry
 * and joins the thread pool.
 */
class ScriptServices {
 public:
  ScriptServices(const ScriptServices&) = delete;
  ScriptServices& operator=(const ScriptServices&) = delete;
  ScriptServices(ScriptServices&&) = delete;
  ScriptServices& operator=(ScriptServices&&) = delete;
  ~ScriptServices();

  /**
   * Build services for an io_context. Probes io_uring once for use_asio_file.
   * @param ioc  Process event loop; Registry uses its executor.
   */
  [[nodiscard]] static std::shared_ptr<ScriptServices> create(
      asio::io_context& ioc,
      ScriptServicesOptions opts = {});

  std::string data_dir;
  std::string ca_bundle;
  /** Snapshot for host.getenv; never re-read process environ after bootstrap. */
  vacps::bootstrap::EnvironmentSnapshot environment;
  asio::thread_pool pool;
  vacps::process::Registry processes;
  /** true after probe_io_uring() at create; File may use random_access_file. */
  bool use_asio_file{false};

  /**
   * Kill tracked children and drop registry entries.
   * Safe to call before destroying the pool (graceful shutdown step 5).
   * Idempotent with respect to destructor (which also shuts down processes).
   */
  void shutdown_processes() noexcept;

 private:
  ScriptServices(asio::io_context& ioc, ScriptServicesOptions opts);

  bool processes_shutdown_{false};
};

}  // namespace vacps::js
