#include "quickjs/module_env.hpp"

#include "fs/io_uring_probe.hpp"
#include "app/log.hpp"

#include <utility>

namespace vacps::js {

ModuleEnv::ModuleEnv(
    asio::io_context& ioc,
    asio::thread_pool& pool,
    ModuleEnvOptions opts)
    : ioc_(ioc),
      pool_(pool),
      data_dir_(std::move(opts.data_dir)),
      ca_bundle_(std::move(opts.ca_bundle)),
      use_stream_file_(vacps::fs::probe_io_uring()),
      path_sandbox_(vacps::fs::PathSandbox::create(data_dir_, std::move(opts.fs_extra_roots))),
      processes_(std::make_unique<vacps::process::Registry>(ioc.get_executor())) {
  log::info(
      "module env ready (data_dir={} fs_content={} fs_roots={})",
      data_dir_,
      use_stream_file_ ? "stream_file/io_uring" : "thread_pool",
      path_sandbox_.roots().size());
}

void ModuleEnv::shutdown() noexcept {
  if (processes_) {
    processes_->shutdown();
  }
}

}  // namespace vacps::js
