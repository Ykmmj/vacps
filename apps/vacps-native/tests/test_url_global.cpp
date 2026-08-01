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
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

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
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

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
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

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

TEST_F(UrlGlobalTest, UsernamePasswordGetters) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-url-userpass";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  auto r = rt->eval(
      R"js(
        const u = new URL('https://alice:s3cret@example.com:8443/p');
        if (u.username !== 'alice') throw new Error('username ' + u.username);
        if (u.password !== 's3cret') throw new Error('password ' + u.password);
        if (u.port !== '8443') throw new Error('port ' + u.port);
        const bare = new URL('https://example.com/');
        if (bare.username !== '') throw new Error('empty user');
        if (bare.password !== '') throw new Error('empty pass');
        'ok';
      )js",
      "url-userpass.js");
  ASSERT_TRUE(r) << r.error().message;
}

TEST_F(UrlGlobalTest, SearchParamsForEachAndIterators) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-url-iter";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  auto r = rt->eval(
      R"js(
        const sp = new URLSearchParams('a=1&b=2&a=3');
        if (sp.size !== 3) throw new Error('size ' + sp.size);

        // forEach(value, name, parent) WHATWG order
        const seen = [];
        sp.forEach(function (value, name, parent) {
          if (parent !== sp) throw new Error('forEach parent');
          seen.push(name + '=' + value);
        });
        if (seen.join(',') !== 'a=1,b=2,a=3') {
          throw new Error('forEach ' + seen.join(','));
        }

        // keys / values / entries
        const keys = [];
        for (const k of sp.keys()) keys.push(k);
        if (keys.join(',') !== 'a,b,a') throw new Error('keys ' + keys.join(','));

        const vals = [];
        for (const v of sp.values()) vals.push(v);
        if (vals.join(',') !== '1,2,3') throw new Error('values ' + vals.join(','));

        const entries = [];
        for (const [k, v] of sp.entries()) entries.push(k + '=' + v);
        if (entries.join(',') !== 'a=1,b=2,a=3') {
          throw new Error('entries ' + entries.join(','));
        }

        // Symbol.iterator === entries semantics (for-of on sp)
        const viaForOf = [];
        for (const pair of sp) {
          viaForOf.push(pair[0] + '=' + pair[1]);
        }
        if (viaForOf.join(',') !== 'a=1,b=2,a=3') {
          throw new Error('for-of ' + viaForOf.join(','));
        }

        // Iterator protocol object shape
        const it = sp.entries();
        const first = it.next();
        if (first.done !== false || first.value[0] !== 'a' || first.value[1] !== '1') {
          throw new Error('next first ' + JSON.stringify(first));
        }
        it.next();
        it.next();
        const end = it.next();
        if (end.done !== true) throw new Error('next end');

        // Live: for-of after mutation on url.searchParams
        const url = new URL('https://x.test/?x=1');
        url.searchParams.append('y', '2');
        const live = [];
        for (const [k, v] of url.searchParams) live.push(k + '=' + v);
        if (live.join(',') !== 'x=1,y=2') throw new Error('live for-of ' + live.join(','));
        if (!url.search.includes('y=2')) throw new Error('live search ' + url.search);

        'ok';
      )js",
      "url-iter.js");
  ASSERT_TRUE(r) << r.error().message;
}

TEST_F(UrlGlobalTest, TextEncoderReturnsUint8Array) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-text-encoder";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  // encoding.d.ts: encode(input?: string): Uint8Array (not bare ArrayBuffer).
  auto r = rt->eval(
      R"js(
        const enc = new TextEncoder();
        if (enc.encoding !== 'utf-8') throw new Error('encoding ' + enc.encoding);
        const u = enc.encode('hi');
        if (!(u instanceof Uint8Array)) {
          throw new Error('expected Uint8Array got ' + Object.prototype.toString.call(u));
        }
        if (u.length !== 2 || u[0] !== 0x68 || u[1] !== 0x69) {
          throw new Error('bytes ' + Array.from(u));
        }
        const empty = enc.encode();
        if (!(empty instanceof Uint8Array) || empty.length !== 0) {
          throw new Error('empty encode');
        }

        // encodeInto: never split multi-byte; WHATWG {read, written}.
        const dest = new Uint8Array(1);
        dest[0] = 0xFF;
        const partial = enc.encodeInto('é', dest);
        if (partial.read !== 0 || partial.written !== 0 || dest[0] !== 0xFF) {
          throw new Error('encodeInto partial ' + JSON.stringify(partial) + ' dest=' + dest[0]);
        }
        const dest2 = new Uint8Array(2);
        const full = enc.encodeInto('é', dest2);
        if (full.read !== 1 || full.written !== 2 || dest2[0] !== 0xC3 || dest2[1] !== 0xA9) {
          throw new Error('encodeInto full ' + JSON.stringify(full) + ' ' + Array.from(dest2));
        }
        'ok';
      )js",
      "text-encoder.js");
  ASSERT_TRUE(r) << r.error().message;
}

TEST_F(UrlGlobalTest, TextDecoderIgnoreBomTrueFalse) {
  asio::io_context ioc{1};
  vacps::js::EngineOptions engine_opts{};
  vacps::js::ScriptServicesOptions services_opts;
  services_opts.data_dir = "/tmp/vacps-text-decoder-bom";
  auto services = vacps::js::ScriptServices::create(ioc, services_opts);

  auto rt_r = vacps::js::ScriptRuntime::create(ioc, engine_opts, services);
  ASSERT_TRUE(rt_r) << rt_r.error().message;
  auto rt = std::move(*rt_r);
  ASSERT_TRUE(vacps::js::install_default_modules(*rt))
      << "install_default_modules failed";

  // WHATWG: ignoreBOM=false (default) consumes leading BOM;
  // ignoreBOM=true keeps U+FEFF as a character.
  auto r = rt->eval(
      R"js(
        const bom = new Uint8Array([0xEF, 0xBB, 0xBF, 0x61]); // BOM + 'a'
        const strip = new TextDecoder('utf-8', { ignoreBOM: false });
        if (strip.ignoreBOM !== false) throw new Error('ignoreBOM prop false');
        const s1 = strip.decode(bom);
        if (s1 !== 'a') throw new Error('default strip got ' + JSON.stringify(s1));

        const keep = new TextDecoder('utf-8', { ignoreBOM: true });
        if (keep.ignoreBOM !== true) throw new Error('ignoreBOM prop true');
        const s2 = keep.decode(bom);
        if (s2.length !== 2 || s2.charCodeAt(0) !== 0xFEFF || s2.charAt(1) !== 'a') {
          throw new Error('keep BOM got codes ' +
            Array.from(s2).map(c => c.charCodeAt(0)).join(','));
        }

        // Streaming: partial BOM must not be committed until 3 bytes available.
        const stream = new TextDecoder('utf-8');
        const p1 = stream.decode(new Uint8Array([0xEF, 0xBB]), { stream: true });
        if (p1 !== '') throw new Error('partial stream ' + JSON.stringify(p1));
        const p2 = stream.decode(new Uint8Array([0xBF, 0x62])); // BOM complete + 'b'
        if (p2 !== 'b') throw new Error('stream strip got ' + JSON.stringify(p2));

        'ok';
      )js",
      "text-decoder-bom.js");
  ASSERT_TRUE(r) << r.error().message;
}
