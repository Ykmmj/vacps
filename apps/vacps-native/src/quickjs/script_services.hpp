#pragma once

/**
 * Process-scoped services for native bindings (ApplicationRuntime layer).
 *
 * Owned by ApplicationRuntime (composition root). ScriptRuntime holds a
 * shared_ptr and exposes it via services().
 *
 * Fields: data_dir, ca_bundle, environment, fs_pool, db_pool, processes
 * (Process backend only — not a JS business object registry), use_asio_file.
 * environment is the bootstrap EnvironmentSnapshot; host.getenv reads only
 * from it. Not product policy (listen/auth) and not QuickJS engine state.
 *
 * JS-owned resources (File/Store/Server/Process handles) are not tracked here;
 * see docs/NATIVE_RESOURCE_OWNERSHIP.md.
 */

#include "bootstrap/environment.hpp"
#include "fs/executor.hpp"
#include "process/runtime.hpp"
#include "storage/db_executor.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace vacps::js {

namespace asio = boost::asio;

/** Construction knobs for ScriptServices (paths + snapshot; pool sizes optional). */
struct ScriptServicesOptions {
  std::string data_dir{"data"};
  std::string ca_bundle;
  /** Process environ captured at BootstrapConfig::fromEnvironment(). */
  vacps::bootstrap::EnvironmentSnapshot environment;
  /** FS namespace + File pool-backend offload threads (default 2). */
  std::size_t fs_pool_threads{2};
  /** Store/SQLite blocking offload threads (default 2). */
  std::size_t db_pool_threads{2};
};

/**
 * Shared process services for vacps:* bindings.
 *
 * Lifetime: ApplicationRuntime creates and retains a shared_ptr; ScriptRuntime
 * receives the same pointer at create. Destructor stops the process registry
 * and joins the thread pools (if not already stopped by ShutdownCoordinator).
 *
 * Two physical pools isolate FS I/O from SQLite blocking work:
 * - fs_pool  → FsExecutor (vacps:fs)
 * - db_pool  → DbExecutor (vacps:store)
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
   * @param ioc  Process event loop; ProcessRuntime uses its executor.
   */
  [[nodiscard]] static std::shared_ptr<ScriptServices> create(
      asio::io_context& ioc,
      ScriptServicesOptions opts = {});

  std::string data_dir;
  std::string ca_bundle;
  /** Snapshot for host.getenv; never re-read process environ after bootstrap. */
  vacps::bootstrap::EnvironmentSnapshot environment;

  /** Blocking offload for vacps:fs (namespace + pool-backend File). */
  asio::thread_pool fs_pool;
  /** Blocking offload for vacps:store / SQLite. */
  asio::thread_pool db_pool;

  /**
   * ProcessRuntime: executor + budget only. Does not own Process instances.
   * JS-owned process::Process holds child state directly.
   */
  vacps::process::ProcessRuntime processes;
  /** true after probe_io_uring() at create; File may use random_access_file. */
  bool use_asio_file{false};

  /** Typed FS view (ioc executor + fs_pool + probe + data_dir). */
  [[nodiscard]] vacps::fs::FsExecutor fs() noexcept {
    return vacps::fs::FsExecutor{
        ioc_executor_,
        fs_pool,
        use_asio_file,
        std::filesystem::path{data_dir},
    };
  }

  /** Typed DB view (db_pool only). */
  [[nodiscard]] vacps::storage::DbExecutor db() noexcept {
    return vacps::storage::DbExecutor{db_pool};
  }

  /**
   * Stop and join fs_pool + db_pool (and any other executors owned here).
   * Call after ScriptRuntime::close() during ordered process shutdown.
   * Idempotent. Process children are killed by ~Process / dispose after
   * FreeContext — no Host process kill-all table.
   */
  void stop_executors() noexcept;

 private:
  ScriptServices(asio::io_context& ioc, ScriptServicesOptions opts);

  asio::any_io_executor ioc_executor_;
  bool executors_stopped_{false};
};

}  // namespace vacps::js
