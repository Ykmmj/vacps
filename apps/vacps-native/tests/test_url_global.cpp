#include "app/log.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/raii/value.hpp"

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
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-url-test";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  // Valid absolute URL (managed-tunnel style hostname).
  auto ok = rt->eval(
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
  auto bad = rt->eval(R"js(new URL('not-a-url'))js", "url-bad.js");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().message.find("Invalid URL"), std::string::npos)
      << bad.error().message;
}

TEST_F(UrlGlobalTest, RelativeWithBase) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-url-test2";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  auto r = rt->eval(
      R"js(
        const u = new URL('/x', 'https://example.com/base/');
        if (u.hostname !== 'example.com') throw new Error(u.hostname);
        if (u.pathname !== '/x') throw new Error(u.pathname);
        'ok';
      )js",
      "url-base.js");
  ASSERT_TRUE(r) << r.error().message;
}

TEST_F(UrlGlobalTest, LiveSearchParamsBidirectional) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-url-live-search";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);

  auto r = rt->eval(
      R"js(
        const url = new URL('https://x.test/?a=1');
        if (url.search !== '?a=1') throw new Error('initial search ' + url.search);
        if (url.searchParams.get('a') !== '1') throw new Error('initial get');

        // Same live object each access.
        if (url.searchParams !== url.searchParams) {
          throw new Error('searchParams identity');
        }

        // Mutation → url.search / href.
        url.searchParams.set('a', '2');
        if (url.search !== '?a=2') throw new Error('after set search=' + url.search);
        if (!String(url.href).includes('?a=2')) throw new Error('href ' + url.href);
        if (url.searchParams.get('a') !== '2') throw new Error('get after set');

        url.searchParams.append('b', '3');
        if (url.searchParams.get('b') !== '3') throw new Error('append get');
        if (!url.search.includes('b=3')) throw new Error('append search ' + url.search);

        // url.search setter → reparse live bag.
        url.search = '?c=9&d=8';
        if (url.search !== '?c=9&d=8') throw new Error('set search ' + url.search);
        if (url.searchParams.get('c') !== '9') throw new Error('reparse c');
        if (url.searchParams.get('d') !== '8') throw new Error('reparse d');
        if (url.searchParams.get('a') !== null) throw new Error('stale a');
        if (url.searchParams.get('b') !== null) throw new Error('stale b');

        // Still the same JS object after search assignment.
        const sp = url.searchParams;
        url.search = '?z=1';
        if (url.searchParams !== sp) throw new Error('identity after search=');
        if (sp.get('z') !== '1') throw new Error('sp after search=');

        // Clear via delete.
        sp.delete('z');
        if (url.search !== '' && url.search !== '?') {
          // Ada clears to empty string (no '?').
          if (url.search !== '') throw new Error('clear search ' + JSON.stringify(url.search));
        }
        if (sp.size !== 0) throw new Error('size after clear ' + sp.size);

        // Standalone URLSearchParams does not touch a URL.
        const alone = new URLSearchParams('x=1');
        alone.set('x', '2');
        if (alone.toString() !== 'x=2') throw new Error('standalone ' + alone.toString());

        'ok';
      )js",
      "url-live-search.js");
  ASSERT_TRUE(r) << r.error().message;
}
