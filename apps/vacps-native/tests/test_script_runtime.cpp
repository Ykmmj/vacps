#include "app/log.hpp"
#include "bootstrap/environment.hpp"
#include "http/script_dispatch.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace asio = boost::asio;

namespace {

// Integration check for vacps:* modules — in tests only, not ScriptRuntime::create.
constexpr std::string_view kModuleSmoke = R"js(
import * as log from "vacps:log";
import * as store from "vacps:store";
import * as host from "vacps:host";
import * as fs from "vacps:fs";
import * as crypto from "vacps:crypto";
import * as process from "vacps:process";

const { Store } = store;
const db = await Store.open(host.dataDir() + "/infra_smoke.db");
await db.exec("SELECT 1;");
const rows = await db.query("SELECT 1 AS n;");
if (!rows || rows.length !== 1 || rows[0].n !== 1) throw new Error("store.query smoke failed");
await db.close();

await fs.mkdir("infra");
{
  const w = await fs.File.open("infra/smoke.txt", fs.O_WRONLY | fs.O_CREAT | fs.O_TRUNC);
  await w.writeText("ok");
  await w.close();
}
{
  const r = await fs.File.open("infra/smoke.txt", fs.O_RDONLY);
  if ((await r.readText()) !== "ok") throw new Error("fs smoke failed");
  await r.close();
}
if (!(await fs.exists("infra/smoke.txt"))) throw new Error("fs.exists smoke failed");

const dig = crypto.sha256Hex("abc");
if (dig !== "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
  throw new Error("sha256 smoke failed: " + dig);
}

const pr = await process.run("/bin/true");
if (pr.exitCode !== 0) throw new Error("process.run smoke failed");

export default 42;
)js";

constexpr std::string_view kBusinessScript = R"js(
import * as host from "vacps:host";
import * as http from "vacps:http";

let server;
export async function initialize() {
  server = new http.Server({ host: "127.0.0.1", port: 18788 });
  await server.listen();
}
export async function shutdown() {
  if (server) await server.close();
  server = undefined;
}

export async function handleRequest(req) {
  if (req.path === "/ping") {
    return {
      status: 200,
      headers: {
        "content-type": "application/json",
        "x-custom": "yes",
        "cache-control": "no-store",
      },
      body: JSON.stringify({ ok: true, id: req.requestId || "" }),
    };
  }
  if (req.path === "/bad-status") {
    return { status: 999, headers: {}, body: "{}" };
  }
  if (req.path === "/not-object") {
    return 42;
  }
  return {
    status: 404,
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ error: "not_found" }),
  };
}
)js";

void write_script(const fs::path& path, std::string_view src) {
  std::ofstream out(path);
  out << src;
}

}  // namespace

class ScriptRuntimeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("off"); }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_script_runtime_test" /
           std::to_string(::getpid()) /
           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(dir_);
    services_opts_.data_dir = dir_.string();
    script_path_ = (dir_ / "biz.mjs").string();
    write_script(script_path_, kBusinessScript);
    // Business script Application requires CP key unless insecure (tests only).
    setenv("VACPS_ALLOW_INSECURE_NO_AUTH", "1", 1);
    // host.getenv reads ScriptServices::environment only (not live getenv).
    services_opts_.environment =
        vacps::bootstrap::EnvironmentSnapshot::from_current_process();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  vacps::js::EngineOptions engine_opts_{};
  vacps::js::ScriptServicesOptions services_opts_{};
  fs::path dir_;
  std::string script_path_;
};

TEST_F(ScriptRuntimeTest, EvalBasic) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt) << rt.error().message;
  auto value = (*rt)->eval("(() => 40 + 2)()", "<test>");
  ASSERT_TRUE(value) << value.error().message;
  auto number =
      vacps::js::converter<std::int32_t>::from_js((*rt)->context().get(), value->get());
  ASSERT_TRUE(number);
  EXPECT_EQ(*number, 42);
}

TEST_F(ScriptRuntimeTest, NativeModulesSmoke) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  bool ok = false;
  std::string err;
  asio::co_spawn(
      ioc,
      [rt, &ok, &err]() -> asio::awaitable<void> {
        auto mod = rt->eval_module(kModuleSmoke, "<module-smoke>");
        if (!mod) {
          err = mod.error().message;
          co_return;
        }
        auto settled = co_await rt->await_value(std::move(*mod));
        if (!settled) {
          err = settled.error().message;
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST_F(ScriptRuntimeTest, LoadAndHandleHttpHeaders) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  bool ok = false;
  std::string err;
  vacps::http::HttpResponse ping;
  vacps::http::HttpResponse not_found;

  asio::co_spawn(
      ioc,
      [rt, &ok, &err, &ping, &not_found, path = script_path_]() -> asio::awaitable<void> {
        auto init = co_await rt->load_and_initialize(path);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        {
          vacps::http::HttpRequest req;
          req.method = "GET";
          req.path = "/ping";
          req.request_id = "rid-1";
          auto r = co_await vacps::http::dispatch_to_script(*rt, std::move(req));
          if (!r) {
            err = r.error().message;
            co_return;
          }
          ping = std::move(*r);
        }
        {
          vacps::http::HttpRequest req;
          req.method = "GET";
          req.path = "/missing";
          auto r = co_await vacps::http::dispatch_to_script(*rt, std::move(req));
          if (!r) {
            err = r.error().message;
            co_return;
          }
          not_found = std::move(*r);
        }

        auto sh = co_await rt->shutdown_script();
        if (!sh) {
          err = sh.error().message;
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;

  EXPECT_EQ(ping.status, 200);
  bool has_custom = false;
  bool has_cache = false;
  for (const auto& [k, v] : ping.headers) {
    if (k == "x-custom" && v == "yes") has_custom = true;
    if (k == "cache-control" && v == "no-store") has_cache = true;
  }
  EXPECT_TRUE(has_custom);
  EXPECT_TRUE(has_cache);
  EXPECT_NE(ping.body.find("\"ok\":true"), std::string::npos);

  EXPECT_EQ(not_found.status, 404);
}

TEST_F(ScriptRuntimeTest, HandleRequestInvalidStatusAndNonObject) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  std::string err_status;
  std::string err_obj;
  bool ok = false;

  asio::co_spawn(
      ioc,
      [rt, &ok, &err_status, &err_obj, path = script_path_]() -> asio::awaitable<void> {
        auto init = co_await rt->load_and_initialize(path);
        if (!init) {
          err_status = init.error().message;
          co_return;
        }
        {
          vacps::http::HttpRequest req;
          req.method = "GET";
          req.path = "/bad-status";
          auto r = co_await vacps::http::dispatch_to_script(*rt, std::move(req));
          if (r) {
            err_status = "expected failure for bad status";
            co_return;
          }
          err_status = r.error().message;
        }
        {
          vacps::http::HttpRequest req;
          req.method = "GET";
          req.path = "/not-object";
          auto r = co_await vacps::http::dispatch_to_script(*rt, std::move(req));
          if (r) {
            err_obj = "expected failure for non-object";
            co_return;
          }
          err_obj = r.error().message;
        }
        // Close inbound Server (signals/accept) so ioc.run() can finish.
        if (auto sh = co_await rt->shutdown_script(); !sh) {
          err_obj = sh.error().message;
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok);
  EXPECT_NE(err_status.find("status"), std::string::npos);
  EXPECT_NE(err_obj.find("object"), std::string::npos);
}

TEST_F(ScriptRuntimeTest, EmptyScriptFails) {
  const auto empty_path = (dir_ / "empty.mjs").string();
  write_script(empty_path, "");
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r);
  auto rt = std::move(*rt_r);
  bool saw_err = false;
  std::string msg;
  asio::co_spawn(
      ioc,
      [rt, empty_path, &saw_err, &msg]() -> asio::awaitable<void> {
        auto init = co_await rt->load_and_initialize(empty_path);
        if (!init) {
          saw_err = true;
          msg = init.error().message;
        }
        co_return;
      },
      asio::detached);
  ioc.run();
  EXPECT_TRUE(saw_err);
  EXPECT_NE(msg.find("empty"), std::string::npos);
}

TEST_F(ScriptRuntimeTest, MissingScriptFails) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r);
  auto rt = std::move(*rt_r);
  bool saw_err = false;
  asio::co_spawn(
      ioc,
      [rt, &saw_err, path = (dir_ / "nope.mjs").string()]() -> asio::awaitable<void> {
        auto init = co_await rt->load_and_initialize(path);
        if (!init) saw_err = true;
        co_return;
      },
      asio::detached);
  ioc.run();
  EXPECT_TRUE(saw_err);
}

TEST_F(ScriptRuntimeTest, TypedArrayViewUsesOffsetAndLength) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto* ctx = (*rt_r)->context().get();

  // new Uint8Array([0,1,2,3,4,5]).subarray(2,5) → [2,3,4]
  auto val = (*rt_r)->eval(
      "(() => { const a = new Uint8Array([0,1,2,3,4,5]); return a.subarray(2, 5); })()",
      "<ta>");
  ASSERT_TRUE(val) << val.error().message;
  auto bytes = vacps::js::bytes_from_js(ctx, val->get());
  ASSERT_TRUE(bytes) << bytes.error().message;
  ASSERT_EQ(bytes->size(), 3u);
  EXPECT_EQ((*bytes)[0], 2);
  EXPECT_EQ((*bytes)[1], 3);
  EXPECT_EQ((*bytes)[2], 4);
}

TEST_F(ScriptRuntimeTest, InterruptBusyLoopWithinBudget) {
  using namespace std::chrono_literals;
  asio::io_context ioc{1};
  vacps::js::EngineOptions opts = engine_opts_;
  opts.js_time_budget = 50ms;
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;

  const auto t0 = std::chrono::steady_clock::now();
  auto value = (*rt_r)->eval("while (true) {}", "<busy>");
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  ASSERT_FALSE(value) << "busy loop should be interrupted";
  // QuickJS throws uncatchable "interrupted" (message may include InternalError).
  EXPECT_NE(value.error().message.find("interrupted"), std::string::npos)
      << value.error().message;
  // Must not hang for seconds; allow some slack for CI load.
  EXPECT_LT(elapsed, 2s) << "interrupt watchdog took too long";
}

TEST_F(ScriptRuntimeTest, InterruptBudgetZeroAllowsShortWork) {
  using namespace std::chrono_literals;
  asio::io_context ioc{1};
  vacps::js::EngineOptions opts = engine_opts_;
  opts.js_time_budget = 0ms;  // watchdog off
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto value = (*rt_r)->eval("(() => 1 + 1)()", "<ok>");
  ASSERT_TRUE(value) << value.error().message;
}

TEST_F(ScriptRuntimeTest, InterruptPromiseMicrotaskBusyLoop) {
  using namespace std::chrono_literals;
  asio::io_context ioc{1};
  vacps::js::EngineOptions opts = engine_opts_;
  opts.js_time_budget = 80ms;
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  bool saw_err = false;
  std::string msg;
  const auto t0 = std::chrono::steady_clock::now();
  asio::co_spawn(
      ioc,
      [rt, &saw_err, &msg]() -> asio::awaitable<void> {
        // Promise then-handler runs an infinite loop during job drain / await.
        auto val = rt->eval(
            R"js((() => Promise.resolve().then(() => { while (true) {} }))())js",
            "<promise-busy>");
        if (!val) {
          saw_err = true;
          msg = val.error().message;
          co_return;
        }
        auto settled = co_await rt->await_value(std::move(*val));
        if (!settled) {
          saw_err = true;
          msg = settled.error().message;
        }
        co_return;
      },
      asio::detached);
  ioc.run();
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  ASSERT_TRUE(saw_err) << "expected interrupt of promise busy loop";
  EXPECT_TRUE(
      msg.find("interrupted") != std::string::npos ||
      msg.find("time budget") != std::string::npos)
      << msg;
  EXPECT_LT(elapsed, 3s) << "promise busy interrupt took too long";
}

