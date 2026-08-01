#include "app/log.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/raii/value.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace asio = boost::asio;

namespace {

#ifndef VACPS_NATIVE_SOURCE_DIR
#error "VACPS_NATIVE_SOURCE_DIR must be defined (CMake)"
#endif

fs::path native_modules_test_script() {
  return fs::path{VACPS_NATIVE_SOURCE_DIR} / "script" / "tests" / "native_modules_test.mjs";
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

class JsNativeApiTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("info"); }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_js_api_test" /
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

/**
 * Load script/tests/native_modules_test.mjs in QuickJS ScriptRuntime and await top-level
 * async tests that exercise every vacps:* surface.
 */
TEST_F(JsNativeApiTest, AllNativeModules) {
  const auto script_path = native_modules_test_script();
  ASSERT_TRUE(fs::exists(script_path)) << "missing " << script_path.string();

  const auto src = read_file(script_path);
  ASSERT_FALSE(src.empty()) << "empty test script";

  asio::io_context ioc{1};
  auto services = vacps::js::ScriptServices::create(ioc, services_opts_);
  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts_, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  bool ok = false;
  std::string err;
  std::int32_t passed = -1;
  std::int32_t total = -1;

  asio::co_spawn(
      ioc,
      [rt, &ok, &err, &passed, &total, src, path = script_path.string()]()
          -> asio::awaitable<void> {
        auto* ctx = rt->context().get();
        // Same as load_and_initialize (read + initialize_from_source): COMPILE_ONLY → EvalFunction → namespace.
        vacps::js::Value compiled{
            ctx,
            JS_Eval(
                ctx,
                src.data(),
                src.size(),
                path.c_str(),
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)};
        if (compiled.is_exception()) {
          err = rt->context().take_exception_error().message;
          co_return;
        }
        auto* mod = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
        vacps::js::Value pending{ctx, JS_EvalFunction(ctx, compiled.release())};
        if (pending.is_exception()) {
          err = rt->context().take_exception_error().message;
          co_return;
        }
        auto settled = co_await rt->await_value(std::move(pending));
        if (!settled) {
          err = settled.error().message;
          co_return;
        }

        vacps::js::Value ns{ctx, JS_GetModuleNamespace(ctx, mod)};
        if (ns.is_exception() || !ns.is_object()) {
          err = "JS_GetModuleNamespace failed";
          co_return;
        }

        // export default { passed, total, ok }
        auto def = vacps::js::Value::get_property_str(ctx, ns.get(), "default");
        if (def.is_exception() || !def.is_object()) {
          err = "module default export missing/not object";
          co_return;
        }

        auto ok_v = vacps::js::Value::get_property_str(ctx, def.get(), "ok");
        auto p_v = vacps::js::Value::get_property_str(ctx, def.get(), "passed");
        auto t_v = vacps::js::Value::get_property_str(ctx, def.get(), "total");
        auto ok_b = vacps::js::converter<bool>::from_js(ctx, ok_v.get());
        auto p_n = vacps::js::converter<std::int32_t>::from_js(ctx, p_v.get());
        auto t_n = vacps::js::converter<std::int32_t>::from_js(ctx, t_v.get());
        if (!ok_b || !p_n || !t_n) {
          err = "summary fields missing (ok/passed/total)";
          co_return;
        }
        passed = *p_n;
        total = *t_n;
        if (!*ok_b) {
          err = "JS suite reported ok=false";
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err << " (passed=" << passed << " total=" << total << ")";
  EXPECT_GT(passed, 0);
  EXPECT_EQ(passed, total);
}
