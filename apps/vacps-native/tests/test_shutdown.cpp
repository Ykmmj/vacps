/**
 * Shutdown state machine smoke tests.
 *
 * Ordered teardown per docs/NATIVE_RESOURCE_OWNERSHIP.md §九:
 *
 *   1. mark Host stopping
 *   2. stop tick
 *   3. JS shutdown() export (business closes Server/Process/Store/…)
 *   4. wait those promises / native async idle
 *   5. drain QuickJS jobs
 *   6. ScriptRuntime::close() — FreeContext / FreeRuntime (finalizers → RAII)
 *   7. stop process backend + pool/executors
 *   8. ioc.stop()
 *
 * Host does not ServerRegistry.stopAll / process business kill-all before JS.
 */

#include "app/log.hpp"
#include "bootstrap/environment.hpp"
#include "http/server.hpp"
#include "quickjs/script_runtime.hpp"
#include "runtime/application_runtime.hpp"
#include "runtime/shutdown_coordinator.hpp"
#include "runtime/tick_loop.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace asio = boost::asio;

namespace {

/** Named steps for documentation / future step-recording hooks. */
enum class ShutdownStep : int {
  MarkStopping = 1,
  StopTick = 2,
  JsShutdownExport = 3,
  WaitShutdownPromises = 4,
  DrainQuickJsJobs = 5,
  ScriptRuntimeClose = 6,
  StopBackendAndExecutors = 7,
  IocStop = 8,
};

// Port chosen per process to reduce parallel-test bind clashes.
inline std::uint16_t test_listen_port(std::uint16_t base) {
  return static_cast<std::uint16_t>(base + (static_cast<unsigned>(::getpid()) % 200));
}

std::string listen_script_source(std::uint16_t port) {
  return std::string(R"js(
import * as http from "vacps:http";

let server;
export async function initialize() {
  server = new http.Server({ host: "127.0.0.1", port: )js") +
         std::to_string(port) +
         R"js( });
  await server.listen();
}
export async function shutdown() {
  if (server) await server.close();
  server = undefined;
}
export async function handleRequest(req) {
  return {
    status: 200,
    headers: { "content-type": "text/plain" },
    body: "ok",
  };
}
export async function tickControlPlane() {}
)js";
}

constexpr std::string_view kInitFailScript = R"js(
export async function initialize() {
  throw new Error("init boom");
}
export async function shutdown() {}
export async function handleRequest() {
  return { status: 500, headers: {}, body: "" };
}
)js";

void write_script(const fs::path& path, std::string_view src) {
  std::ofstream out(path);
  out << src;
}

class ShutdownTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("off"); }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_shutdown_test" /
           std::to_string(::getpid()) /
           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(dir_);
    services_opts_.data_dir = dir_.string();
    services_opts_.environment =
        vacps::bootstrap::EnvironmentSnapshot::from_current_process();
    setenv("VACPS_ALLOW_INSECURE_NO_AUTH", "1", 1);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  vacps::js::EngineOptions engine_opts_{};
  vacps::js::ScriptServicesOptions services_opts_{};
  fs::path dir_;
};

}  // namespace

TEST_F(ShutdownTest, ExpectedOrderConstants) {
  EXPECT_EQ(static_cast<int>(ShutdownStep::MarkStopping), 1);
  EXPECT_EQ(static_cast<int>(ShutdownStep::StopTick), 2);
  EXPECT_EQ(static_cast<int>(ShutdownStep::JsShutdownExport), 3);
  EXPECT_EQ(static_cast<int>(ShutdownStep::WaitShutdownPromises), 4);
  EXPECT_EQ(static_cast<int>(ShutdownStep::DrainQuickJsJobs), 5);
  EXPECT_EQ(static_cast<int>(ShutdownStep::ScriptRuntimeClose), 6);
  EXPECT_EQ(static_cast<int>(ShutdownStep::StopBackendAndExecutors), 7);
  EXPECT_EQ(static_cast<int>(ShutdownStep::IocStop), 8);
}

/** Server RAII: destructor stops accept without Host registry. */
TEST_F(ShutdownTest, ServerDestructorClosesAccept) {
  asio::io_context ioc{1};

  struct NoopHandler : vacps::http::IRequestHandler {
    asio::awaitable<vacps::Result<vacps::http::HttpResponse>> handle(
        vacps::http::HttpRequest) override {
      vacps::http::HttpResponse res;
      res.status = 200;
      co_return res;
    }
  };

  auto handler = std::make_shared<NoopHandler>();
  vacps::http::ListenEndpoint ep;
  ep.host = "127.0.0.1";
  ep.port = test_listen_port(18801);
  auto server = std::make_shared<vacps::http::Server>(ioc, ep, handler);
  ASSERT_TRUE(server->start()) << "bind failed";
  EXPECT_TRUE(server->is_open());
  server->close();
  EXPECT_FALSE(server->is_open());
  server.reset();
  ioc.poll();  // drain accept loop cancellation
}

TEST_F(ShutdownTest, CoordinatorStopsServerAndClosesRuntimeWithoutHang) {
  const auto script_path = (dir_ / "listen.mjs").string();
  write_script(script_path, listen_script_source(test_listen_port(18799)));

  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  vacps::runtime::TickLoop tick(ioc);
  vacps::runtime::ShutdownCoordinator shutdown(ioc);
  shutdown.arm_signals(rt, tick.timer());

  std::string err;
  bool init_ok = false;

  asio::co_spawn(
      ioc,
      [rt, &shutdown, &tick, &err, &init_ok, path = script_path]()
          -> asio::awaitable<void> {
        auto init = co_await vacps::js::load_and_initialize(*rt, path);
        if (!init) {
          err = init.error().message;
          shutdown.request_stop(rt, tick.timer());
          co_return;
        }
        init_ok = true;
        // JS owns Server; Host only calls request_stop → JS shutdown closes it.
        shutdown.request_stop(rt, tick.timer());
        co_return;
      },
      asio::detached);

  ioc.run();

  EXPECT_TRUE(init_ok) << err;
  EXPECT_TRUE(shutdown.stopping());
  EXPECT_TRUE(rt->closed());
  EXPECT_TRUE(rt->closing());
  EXPECT_FALSE(rt->ok());
}

TEST_F(ShutdownTest, InitFailureUsesSameShutdownPath) {
  const auto script_path = (dir_ / "fail.mjs").string();
  write_script(script_path, kInitFailScript);

  vacps::runtime::ApplicationRuntimeOptions opts;
  opts.services = services_opts_;
  opts.engine = engine_opts_;
  opts.tick_interval = std::chrono::hours{24};

  vacps::runtime::ApplicationRuntime app(std::move(opts));
  ASSERT_TRUE(app.start(script_path));

  // run() drives init fail → request_stop → ordered close; must return.
  const int code = app.run();
  EXPECT_EQ(code, EXIT_FAILURE);
  EXPECT_TRUE(app.stopping());
}

TEST_F(ShutdownTest, ApplicationRuntimeStopWithListeningServer) {
  const auto script_path = (dir_ / "app_listen.mjs").string();
  write_script(script_path, listen_script_source(test_listen_port(18821)));

  vacps::runtime::ApplicationRuntimeOptions opts;
  opts.services = services_opts_;
  opts.engine = engine_opts_;
  opts.tick_interval = std::chrono::hours{24};

  vacps::runtime::ApplicationRuntime app(std::move(opts));
  ASSERT_TRUE(app.start(script_path));

  std::thread stopper([&app] {
    // Allow initialize() + listen to complete, then request ordered stop.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    app.stop();
  });

  const int code = app.run();
  stopper.join();
  EXPECT_EQ(code, EXIT_SUCCESS);
  EXPECT_TRUE(app.stopping());
}
