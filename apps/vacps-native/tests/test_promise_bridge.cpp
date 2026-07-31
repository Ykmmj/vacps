#include "app/log.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstring>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace asio = boost::asio;

class PromiseBridgeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("off"); }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_promise_bridge_test" /
           std::to_string(::getpid()) /
           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(dir_);
    services_opts_.data_dir = dir_.string();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  vacps::js::EngineOptions engine_opts_{};
  vacps::js::ScriptServicesOptions services_opts_{};
  fs::path dir_;
};

TEST_F(PromiseBridgeTest, ResolveValueAndNotifyProgress) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  auto* ctx = rt->context().get();

  const auto gen0 = rt->progress_generation();
  bool ok = false;
  std::string err;
  std::int32_t got = -1;

  asio::co_spawn(
      ioc,
      [rt, ctx, &ok, &err, &got]() -> asio::awaitable<void> {
        vacps::js::Value promise{
            ctx,
            vacps::js::spawn_js_promise(
                ctx,
                rt.get(),
                [](JSContext* c, vacps::js::PromiseBridge& bridge)
                    -> asio::awaitable<void> {
                  // Yield once so await_settled must sleep on progress, not busy-spin.
                  auto ex = co_await asio::this_coro::executor;
                  co_await asio::post(ex, asio::use_awaitable);
                  bridge.resolve(vacps::js::converter<std::int32_t>::to_js(c, 42));
                  co_return;
                })};
        if (promise.is_exception()) {
          err = "spawn threw";
          co_return;
        }
        auto settled = co_await rt->await_value(std::move(promise));
        if (!settled) {
          err = settled.error().message;
          co_return;
        }
        auto n = vacps::js::converter<std::int32_t>::from_js(ctx, settled->get());
        if (!n) {
          err = "not a number";
          co_return;
        }
        got = *n;
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
  EXPECT_EQ(got, 42);
  EXPECT_GT(rt->progress_generation(), gen0);
}

TEST_F(PromiseBridgeTest, ExceptionRejectsPromise) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  auto* ctx = rt->context().get();

  bool rejected = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [rt, ctx, &rejected, &err]() -> asio::awaitable<void> {
        vacps::js::Value promise{
            ctx,
            vacps::js::spawn_js_promise(
                ctx,
                rt.get(),
                [](JSContext*, vacps::js::PromiseBridge&) -> asio::awaitable<void> {
                  throw std::runtime_error("bridge-test-boom");
                  co_return;
                })};
        auto settled = co_await rt->await_value(std::move(promise));
        if (settled) {
          err = "expected rejection";
          co_return;
        }
        err = settled.error().message;
        rejected = err.find("bridge-test-boom") != std::string::npos;
        co_return;
      },
      asio::detached);

  ioc.run();
  EXPECT_TRUE(rejected) << err;
}

TEST_F(PromiseBridgeTest, UnsettledWorkGetsDefensiveReject) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  auto* ctx = rt->context().get();

  bool rejected = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [rt, ctx, &rejected, &err]() -> asio::awaitable<void> {
        vacps::js::Value promise{
            ctx,
            vacps::js::spawn_js_promise(
                ctx,
                rt.get(),
                [](JSContext*, vacps::js::PromiseBridge&) -> asio::awaitable<void> {
                  // Intentionally no settle.
                  co_return;
                })};
        auto settled = co_await rt->await_value(std::move(promise));
        if (settled) {
          err = "expected defensive reject";
          co_return;
        }
        err = settled.error().message;
        rejected = err.find("without settling") != std::string::npos;
        co_return;
      },
      asio::detached);

  ioc.run();
  EXPECT_TRUE(rejected) << err;
}

TEST_F(PromiseBridgeTest, SettleOnceKeepsFirstResult) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  auto* ctx = rt->context().get();

  bool ok = false;
  std::string err;
  std::int32_t got = -1;

  asio::co_spawn(
      ioc,
      [rt, ctx, &ok, &err, &got]() -> asio::awaitable<void> {
        vacps::js::Value promise{
            ctx,
            vacps::js::spawn_js_promise(
                ctx,
                rt.get(),
                [](JSContext* c, vacps::js::PromiseBridge& bridge)
                    -> asio::awaitable<void> {
                  bridge.resolve(vacps::js::converter<std::int32_t>::to_js(c, 7));
                  // Must not override.
                  bridge.reject_message("should-be-ignored");
                  bridge.resolve(vacps::js::converter<std::int32_t>::to_js(c, 99));
                  co_return;
                })};
        auto settled = co_await rt->await_value(std::move(promise));
        if (!settled) {
          err = settled.error().message;
          co_return;
        }
        auto n = vacps::js::converter<std::int32_t>::from_js(ctx, settled->get());
        if (!n) {
          err = "not a number";
          co_return;
        }
        got = *n;
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
  EXPECT_EQ(got, 7);
}

TEST_F(PromiseBridgeTest, ExplicitRejectWithError) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  auto* ctx = rt->context().get();

  bool rejected = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [rt, ctx, &rejected, &err]() -> asio::awaitable<void> {
        vacps::js::Value promise{
            ctx,
            vacps::js::spawn_js_promise(
                ctx,
                rt.get(),
                [](JSContext*, vacps::js::PromiseBridge& bridge)
                    -> asio::awaitable<void> {
                  bridge.reject(vacps::Error{"path_not_found_test"});
                  co_return;
                })};
        auto settled = co_await rt->await_value(std::move(promise));
        if (settled) {
          err = "expected reject";
          co_return;
        }
        err = settled.error().message;
        rejected = err.find("path_not_found_test") != std::string::npos;
        co_return;
      },
      asio::detached);

  ioc.run();
  EXPECT_TRUE(rejected) << err;
}

TEST_F(PromiseBridgeTest, DrainJobsBudgetedCapsPerCall) {
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto host = std::move(*rt_r);
  auto* ctx = host->context().get();
  JSRuntime* qjs = host->runtime().get();

  // Enqueue many microtasks without ScriptRuntime::eval full drain.
  constexpr const char* kCode =
      "globalThis.__pb_n = 0;"
      "for (let i = 0; i < 40; i++) {"
      "  Promise.resolve().then(() => { globalThis.__pb_n++; });"
      "}";
  vacps::js::Value evaled{
      ctx, JS_Eval(ctx, kCode, std::strlen(kCode), "<budget>", JS_EVAL_TYPE_GLOBAL)};
  ASSERT_FALSE(evaled.is_exception()) << host->context().take_exception_error().message;
  ASSERT_TRUE(JS_IsJobPending(qjs));

  auto first = host->runtime().drain_jobs_budgeted(10);
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_EQ(*first, 10u);
  EXPECT_TRUE(JS_IsJobPending(qjs));

  auto rest = host->runtime().drain_jobs_budgeted(1000);
  ASSERT_TRUE(rest) << rest.error().message;
  EXPECT_GE(*rest, 1u);
  EXPECT_FALSE(JS_IsJobPending(qjs));

  // Verify all then-handlers ran.
  vacps::js::Value n_val{ctx, JS_Eval(ctx, "globalThis.__pb_n", 17, "<n>", JS_EVAL_TYPE_GLOBAL)};
  ASSERT_FALSE(n_val.is_exception());
  auto n = vacps::js::converter<std::int32_t>::from_js(ctx, n_val.get());
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 40);
}

TEST_F(PromiseBridgeTest, ProcessRunThroughBridgeStillWorks) {
  // Integration: real vacps:process path uses spawn_js_promise.
  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  bool ok = false;
  std::string err;
  static constexpr std::string_view kMod = R"js(
import * as process from "vacps:process";
const pr = await process.run("/bin/true");
if (pr.exitCode !== 0) throw new Error("exit " + pr.exitCode);
export default pr.exitCode;
)js";

  asio::co_spawn(
      ioc,
      [rt, &ok, &err]() -> asio::awaitable<void> {
        auto mod = rt->eval_module(kMod, "<process-bridge>");
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
