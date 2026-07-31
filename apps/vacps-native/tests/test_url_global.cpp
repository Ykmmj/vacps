#include "app/log.hpp"
#include "quickjs/host.hpp"
#include "quickjs/value.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <string>

namespace asio = boost::asio;

class UrlGlobalTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("info"); }
};

TEST_F(UrlGlobalTest, ParsesAbsoluteHttpsAndRejectsGarbage) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ModuleEnvOptions module_opts{};
  module_opts.data_dir = "/tmp/vacps-url-test";
  auto host_r = vacps::js::Host::create(ioc, engine_opts, module_opts);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  // Valid absolute URL (managed-tunnel style hostname).
  auto ok = host->eval(
      R"js(
        const u = new URL('https://vacps-df15d16eb52d.803800.xyz/path?q=1#h');
        if (u.protocol !== 'https:') throw new Error('protocol ' + u.protocol);
        if (u.hostname !== 'vacps-df15d16eb52d.803800.xyz') throw new Error('host ' + u.hostname);
        if (!String(u.href).startsWith('https://')) throw new Error('href ' + u.href);
        if (typeof URL.canParse !== 'function') throw new Error('no canParse');
        if (!URL.canParse('https://example.com')) throw new Error('canParse true');
        if (URL.canParse('not-a-url')) throw new Error('canParse false');
        'ok';
      )js",
      "url-ok.js");
  ASSERT_TRUE(ok) << ok.error().message;

  // Invalid URL throws TypeError (Zod z.url() relies on this).
  auto bad = host->eval(R"js(new URL('not-a-url'))js", "url-bad.js");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().message.find("Invalid URL"), std::string::npos)
      << bad.error().message;
}

TEST_F(UrlGlobalTest, RelativeWithBase) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ModuleEnvOptions module_opts{};
  module_opts.data_dir = "/tmp/vacps-url-test2";
  auto host_r = vacps::js::Host::create(ioc, engine_opts, module_opts);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  auto r = host->eval(
      R"js(
        const u = new URL('/x', 'https://example.com/base/');
        if (u.hostname !== 'example.com') throw new Error(u.hostname);
        if (u.pathname !== '/x') throw new Error(u.pathname);
        'ok';
      )js",
      "url-base.js");
  ASSERT_TRUE(r) << r.error().message;
}
