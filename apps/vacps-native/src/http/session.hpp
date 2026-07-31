#pragma once

#include "http/routes.hpp"
#include "http/script_dispatch.hpp"
#include "app/log.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace vacps::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class Server;  // session lifetime tracked by Server for graceful drain

/** Per-operation idle timeout for read/write. */
inline constexpr std::chrono::seconds kSessionIoTimeout{30};
/** Max HTTP body size (bytes). */
inline constexpr std::uint64_t kMaxBodyBytes = 1 * 1024 * 1024;

inline bool is_benign_disconnect(const beast::error_code& ec) {
  return ec == http::error::end_of_stream || ec == asio::error::eof ||
         ec == asio::error::connection_reset || ec == asio::error::broken_pipe ||
         ec == beast::error::timeout || ec == http::error::body_limit;
}

/** TCP session: pure transport; dispatches to script via http::dispatch_to_script. */
class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(
      tcp::socket socket,
      std::shared_ptr<vacps::js::Host> script,
      std::weak_ptr<Server> server = {});

  void run();

  /** Best-effort cancel of in-flight I/O (graceful shutdown). */
  void cancel() noexcept;

 private:
  void notify_finished() noexcept;
  asio::awaitable<void> do_session();
  asio::awaitable<http::response<http::string_body>> dispatch(
      http::request<http::string_body> req);

  beast::tcp_stream stream_;
  std::shared_ptr<vacps::js::Host> script_;
  std::weak_ptr<Server> server_;
  bool finished_{false};
};

}  // namespace vacps::http
