#pragma once

/**
 * Process-scoped wiring for vacps:* native modules.
 *
 * Owned for lifetime by Host (composition root), but is NOT the JS engine.
 * Modules use env_from(ctx); they must not treat Host as a service locator
 * for fs/process/http policy.
 *
 * Boundary:
 * - Host  = QuickJS + Asio glue (eval, await, offload pool, async scope)
 * - ModuleEnv = capability backends (sandbox, process registry, agent paths)
 */

#include "fs/sandbox.hpp"
#include "process/registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {

namespace asio = boost::asio;

struct ModuleEnvOptions {
  std::string data_dir{"data"};
  std::string ca_bundle;
  std::vector<std::string> fs_extra_roots;
};

/**
 * Backends used by vacps:fs / vacps:process / vacps:http / vacps:host.
 * Construct once at process start; destroy after JS context is gone.
 */
class ModuleEnv {
 public:
  ModuleEnv(
      asio::io_context& ioc,
      asio::thread_pool& pool,
      ModuleEnvOptions opts);

  ModuleEnv(const ModuleEnv&) = delete;
  ModuleEnv& operator=(const ModuleEnv&) = delete;

  [[nodiscard]] asio::io_context& ioc() noexcept { return ioc_; }
  [[nodiscard]] asio::thread_pool& pool() noexcept { return pool_; }

  [[nodiscard]] const std::string& data_dir() const noexcept { return data_dir_; }
  [[nodiscard]] const std::string& ca_bundle() const noexcept { return ca_bundle_; }
  [[nodiscard]] bool use_stream_file() const noexcept { return use_stream_file_; }

  [[nodiscard]] vacps::fs::PathSandbox& path_sandbox() noexcept { return path_sandbox_; }
  [[nodiscard]] const vacps::fs::PathSandbox& path_sandbox() const noexcept {
    return path_sandbox_;
  }

  [[nodiscard]] vacps::process::Registry& processes() noexcept { return *processes_; }

  void shutdown() noexcept;

 private:
  asio::io_context& ioc_;
  asio::thread_pool& pool_;
  std::string data_dir_;
  std::string ca_bundle_;
  bool use_stream_file_{false};
  vacps::fs::PathSandbox path_sandbox_;
  std::unique_ptr<vacps::process::Registry> processes_;
};

}  // namespace vacps::js
