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
      fs_pool(std::max<std::size_t>(1, opts.fs_pool_threads)),
      db_pool(std::max<std::size_t>(1, opts.db_pool_threads)),
      processes(ioc.get_executor()),
      use_asio_file(vacps::fs::probe_io_uring()),
      ioc_executor_(ioc.get_executor()) {}

ScriptServices::~ScriptServices() {
  // After FreeContext: Process dispose/dtor already killed children.
  stop_executors();
}

std::shared_ptr<ScriptServices> ScriptServices::create(
    asio::io_context& ioc,
    ScriptServicesOptions opts) {
  const std::size_t fs_threads = std::max<std::size_t>(1, opts.fs_pool_threads);
  const std::size_t db_threads = std::max<std::size_t>(1, opts.db_pool_threads);
  auto services = std::shared_ptr<ScriptServices>(
      new ScriptServices(ioc, std::move(opts)));
  log::info(
      "script services ready (data_dir={} fs_file={} fs_pool_threads={} db_pool_threads={})",
      services->data_dir,
      services->use_asio_file ? "random_access_file/io_uring" : "thread_pool",
      fs_threads,
      db_threads);
  return services;
}

void ScriptServices::stop_executors() noexcept {
  if (executors_stopped_) {
    return;
  }
  executors_stopped_ = true;
  fs_pool.stop();
  db_pool.stop();
  fs_pool.join();
  db_pool.join();
}

}  // namespace vacps::js
