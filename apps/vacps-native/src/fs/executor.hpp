#pragma once

/**
 * FsExecutor — I/O environment for vacps:fs File + namespace async.
 *
 * Bundles the host knobs previously passed as separate open/async args
 * (ioc executor, thread_pool, use_asio_file, data_dir).
 */

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/thread_pool.hpp>

#include <filesystem>

namespace vacps::fs {

namespace asio = boost::asio;

/**
 * Process-scoped FS execution context.
 *
 * - ioc_executor: Asio random_access_file affinity (when use_asio_file)
 * - pool: blocking offload (namespace ops + pool-backend File I/O)
 * - use_asio_file: true only after probe_io_uring() succeeded
 * - data_dir: default relative-path resolve base
 */
struct FsExecutor {
  asio::any_io_executor ioc_executor{};
  asio::thread_pool& pool;
  bool use_asio_file{false};
  std::filesystem::path data_dir{};
};

}  // namespace vacps::fs
