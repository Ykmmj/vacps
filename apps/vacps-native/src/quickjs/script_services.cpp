#include "quickjs/script_services.hpp"

#include "app/log.hpp"
#include "fs/io_uring_probe.hpp"

#include <algorithm>
#include <utility>

namespace vacps::js {

ScriptServices::ScriptServices(asio::io_context& ioc, ScriptServicesOptions opts)
    : data_dir(std::move(opts.data_dir)),
      ca_bundle(std::move(opts.ca_bundle)),
      environment(std::move(opts.environment)),
      pool(std::max<std::size_t>(1, opts.pool_threads)),
      processes(ioc.get_executor()),
      use_asio_file(vacps::fs::probe_io_uring()) {}

ScriptServices::~ScriptServices() {
  shutdown_processes();
  pool.stop();
  pool.join();
}

std::shared_ptr<ScriptServices> ScriptServices::create(
    asio::io_context& ioc,
    ScriptServicesOptions opts) {
  const std::size_t threads = std::max<std::size_t>(1, opts.pool_threads);
  auto services = std::shared_ptr<ScriptServices>(
      new ScriptServices(ioc, std::move(opts)));
  log::info(
      "script services ready (data_dir={} fs_file={} pool_threads={})",
      services->data_dir,
      services->use_asio_file ? "random_access_file/io_uring" : "thread_pool",
      threads);
  return services;
}

void ScriptServices::shutdown_processes() noexcept {
  if (processes_shutdown_) {
    return;
  }
  processes_shutdown_ = true;
  processes.shutdown();
}

}  // namespace vacps::js
