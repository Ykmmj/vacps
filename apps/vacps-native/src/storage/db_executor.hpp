#pragma once

/**
 * DbExecutor — SQLite / Store blocking work only.
 *
 * Separate from FsExecutor so FS and DB thread-pool sizing and isolation
 * are independent (ScriptServices owns fs_pool + db_pool).
 */

#include <boost/asio/thread_pool.hpp>

namespace vacps::storage {

namespace asio = boost::asio;

/** Host view for offloading Store/Database blocking calls. */
struct DbExecutor {
  asio::thread_pool& pool;
};

}  // namespace vacps::storage
