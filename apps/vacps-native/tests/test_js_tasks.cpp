#include "app/config.hpp"
#include "app/log.hpp"
#include "http/script_dispatch.hpp"
#include "quickjs/host.hpp"
#include "storage/db.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace asio = boost::asio;

#ifndef VACPS_NATIVE_SOURCE_DIR
#error "VACPS_NATIVE_SOURCE_DIR required"
#endif

namespace {

fs::path business_script() {
  return fs::path{VACPS_NATIVE_SOURCE_DIR} / "script" / "dist" / "vacps.mjs";
}

}  // namespace

class JsTasksTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { vacps::log::init("info"); }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_js_tasks" / std::to_string(::getpid()) /
           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(dir_);
    cfg_.data_dir = dir_.string();
    cfg_.log_level = "info";
    // Match default backend id in agent-config when BACKEND_ID unset.
    setenv("BACKEND_ID", "local", 1);
    // Integration tests dispatch unsigned HTTP; production requires a CP key.
    setenv("VACPS_ALLOW_INSECURE_NO_AUTH", "1", 1);
  }

  vacps::Config cfg_{};
  fs::path dir_;
};

TEST_F(JsTasksTest, EnqueueCommandAndCompleteViaPump) {
  ASSERT_TRUE(fs::exists(business_script())) << business_script();

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;
  int get_status = 0;
  std::string get_body;

  asio::co_spawn(
      ioc,
      [host, &ok, &err, &get_status, &get_body, script = business_script().string()]()
          -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        const std::string task_id = "11111111-1111-4111-8111-111111111111";
        const std::string body = R"({
          "kind": "command",
          "backend_id": "local",
          "task_id": ")" + task_id + R"(",
          "source": "api",
          "program": "/bin/echo",
          "arguments": ["task-hello"],
          "timeout_seconds": 30,
          "working_directory": "/tmp",
          "profile": "full",
          "output": {
            "capture_stdout": true,
            "capture_stderr": true,
            "preview_max_bytes": 8192,
            "retention_seconds": 86400,
            "hard_max_bytes": 10485760
          }
        })";

        vacps::http::ScriptHttpRequest post;
        post.method = "POST";
        post.path = "/tasks";
        post.body = body;
        post.headers.emplace_back("content-type", "application/json");
        auto pr = co_await vacps::http::dispatch_to_script(*host, std::move(post));
        if (!pr) {
          err = pr.error().message;
          co_return;
        }
        if (pr->status != 202) {
          err = "POST /tasks status " + std::to_string(pr->status) + " body=" + pr->body;
          co_return;
        }

        // Pump
        auto tick = co_await host->invoke_export("tickControlPlane", 0, nullptr);
        if (!tick) {
          err = "tick: " + tick.error().message;
          co_return;
        }

        vacps::http::ScriptHttpRequest get;
        get.method = "GET";
        get.path = "/tasks/" + task_id;
        auto gr = co_await vacps::http::dispatch_to_script(*host, std::move(get));
        if (!gr) {
          err = gr.error().message;
          co_return;
        }
        get_status = gr->status;
        get_body = gr->body;
        if (gr->status != 200) {
          err = "GET status " + std::to_string(gr->status) + " " + gr->body;
          co_return;
        }
        if (gr->body.find("succeeded") == std::string::npos &&
            gr->body.find("\"status\":\"succeeded\"") == std::string::npos) {
          // allow either shape
          if (gr->body.find("task-hello") == std::string::npos &&
              gr->body.find("succeeded") == std::string::npos) {
            err = "unexpected task body: " + gr->body;
            co_return;
          }
        }

        auto sh = co_await host->shutdown_script();
        if (!sh) {
          err = sh.error().message;
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err << " get_status=" << get_status << " body=" << get_body;
}

TEST_F(JsTasksTest, RetryAndCrashRecovery) {
  ASSERT_TRUE(fs::exists(business_script()));

  // ── Crash recovery: plant a running row, then boot script ──
  {
    auto db = vacps::Database::open((dir_ / "agent.db").string());
    ASSERT_TRUE(db) << db.error().message;
    // Minimal schema matching migration v2
    ASSERT_TRUE(db->exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT);"
        "CREATE TABLE IF NOT EXISTS agent_state (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS tasks ("
        " id TEXT PRIMARY KEY, backend_id TEXT, kind TEXT, status TEXT, profile TEXT,"
        " input_json TEXT, result_json TEXT, error_json TEXT,"
        " cancel_requested INTEGER DEFAULT 0,"
        " created_at TEXT, started_at TEXT, finished_at TEXT, updated_at TEXT);"
        "CREATE TABLE IF NOT EXISTS task_logs ("
        " task_id TEXT NOT NULL, sequence INTEGER NOT NULL, stream TEXT, data TEXT, created_at TEXT,"
        " PRIMARY KEY(task_id, sequence));"
        "CREATE TABLE IF NOT EXISTS request_nonces (nonce TEXT PRIMARY KEY, expires_at INTEGER);"));
    ASSERT_TRUE(db->exec(
        "INSERT INTO schema_migrations(version, applied_at) VALUES (1, 't'), (2, 't');"));
    const std::string input =
        R"({"kind":"command","backend_id":"local","task_id":"22222222-2222-4222-8222-222222222222","source":"api","program":"/bin/true","arguments":[],"timeout_seconds":10,"working_directory":"/tmp","profile":"full","output":{"capture_stdout":true,"capture_stderr":true,"preview_max_bytes":8192,"retention_seconds":86400,"hard_max_bytes":10485760}})";
    ASSERT_TRUE(db->execute(
        "INSERT INTO tasks(id, backend_id, kind, status, profile, input_json, cancel_requested, created_at, started_at, updated_at) "
        "VALUES(?,?,?,?,?,?,0,?,?,?);",
        {vacps::sql_text("22222222-2222-4222-8222-222222222222"),
         vacps::sql_text("local"),
         vacps::sql_text("command"),
         vacps::sql_text("running"),
         vacps::sql_text("full"),
         vacps::sql_text(input),
         vacps::sql_text("2020-01-01T00:00:00.000Z"),
         vacps::sql_text("2020-01-01T00:00:01.000Z"),
         vacps::sql_text("2020-01-01T00:00:01.000Z")}));
  }

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [host, &ok, &err, script = business_script().string()]() -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        // Recovered task should be failed / agent_restarted
        vacps::http::ScriptHttpRequest get;
        get.method = "GET";
        get.path = "/tasks/22222222-2222-4222-8222-222222222222";
        auto gr = co_await vacps::http::dispatch_to_script(*host, std::move(get));
        if (!gr || gr->status != 200) {
          err = gr ? gr->body : gr.error().message;
          co_return;
        }
        if (gr->body.find("agent_restarted") == std::string::npos &&
            gr->body.find("failed") == std::string::npos) {
          err = "expected agent_restarted: " + gr->body;
          co_return;
        }

        // Retry → new queued task
        vacps::http::ScriptHttpRequest retry;
        retry.method = "POST";
        retry.path = "/tasks/22222222-2222-4222-8222-222222222222/retry";
        auto rr = co_await vacps::http::dispatch_to_script(*host, std::move(retry));
        if (!rr || rr->status != 202) {
          err = rr ? ("retry " + std::to_string(rr->status) + " " + rr->body) : rr.error().message;
          co_return;
        }
        if (rr->body.find("retry_of_task_id") == std::string::npos) {
          err = "retry body: " + rr->body;
          co_return;
        }

        auto tick = co_await host->invoke_export("tickControlPlane", 0, nullptr);
        if (!tick) {
          err = tick.error().message;
          co_return;
        }

        auto sh = co_await host->shutdown_script();
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
}

TEST_F(JsTasksTest, ExecAndFsRoutes) {
  ASSERT_TRUE(fs::exists(business_script()));

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;
  const auto file_path = (dir_ / "hello.txt").string();

  asio::co_spawn(
      ioc,
      [host, &ok, &err, script = business_script().string(), file_path]()
          -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        // POST /exec/command
        vacps::http::ScriptHttpRequest exec;
        exec.method = "POST";
        exec.path = "/exec/command";
        exec.headers.emplace_back("content-type", "application/json");
        exec.body = R"({
          "program": "/bin/echo",
          "arguments": ["exec-ok"],
          "timeout_ms": 5000,
          "working_directory": "/tmp"
        })";
        auto er = co_await vacps::http::dispatch_to_script(*host, std::move(exec));
        if (!er || er->status != 200) {
          err = er ? ("exec " + std::to_string(er->status) + " " + er->body)
                   : er.error().message;
          co_return;
        }
        if (er->body.find("exec-ok") == std::string::npos) {
          err = "exec body missing output: " + er->body;
          co_return;
        }

        // POST /fs/write
        vacps::http::ScriptHttpRequest write;
        write.method = "POST";
        write.path = "/fs/write";
        write.headers.emplace_back("content-type", "application/json");
        write.body = std::string(R"({
          "path": ")") +
                     file_path + R"(",
          "content": "native-fs-hello",
          "mode": "create_or_overwrite"
        })";
        auto wr = co_await vacps::http::dispatch_to_script(*host, std::move(write));
        if (!wr || wr->status != 200) {
          err = wr ? ("write " + std::to_string(wr->status) + " " + wr->body)
                   : wr.error().message;
          co_return;
        }

        // GET /fs/read
        vacps::http::ScriptHttpRequest read;
        read.method = "GET";
        read.path = "/fs/read";
        read.query = "path=" + file_path;
        auto rr = co_await vacps::http::dispatch_to_script(*host, std::move(read));
        if (!rr || rr->status != 200) {
          err = rr ? ("read " + std::to_string(rr->status) + " " + rr->body)
                   : rr.error().message;
          co_return;
        }
        if (rr->body.find("native-fs-hello") == std::string::npos) {
          err = "read body: " + rr->body;
          co_return;
        }

        auto sh = co_await host->shutdown_script();
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
}

TEST_F(JsTasksTest, MetricsAndScheduler) {
  ASSERT_TRUE(fs::exists(business_script()));

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [host, &ok, &err, script = business_script().string()]() -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        vacps::http::ScriptHttpRequest health;
        health.method = "GET";
        health.path = "/health";
        auto hr = co_await vacps::http::dispatch_to_script(*host, std::move(health));
        if (!hr || hr->status != 200) {
          err = hr ? hr->body : hr.error().message;
          co_return;
        }
        if (hr->body.find("backendId") == std::string::npos &&
            hr->body.find("\"ok\"") == std::string::npos) {
          err = "health shape: " + hr->body;
          co_return;
        }

        vacps::http::ScriptHttpRequest metrics;
        metrics.method = "GET";
        metrics.path = "/metrics";
        auto mr = co_await vacps::http::dispatch_to_script(*host, std::move(metrics));
        if (!mr || mr->status != 200) {
          err = mr ? mr->body : mr.error().message;
          co_return;
        }
        if (mr->body.find("cpu") == std::string::npos ||
            mr->body.find("queue") == std::string::npos) {
          err = "metrics shape: " + mr->body;
          co_return;
        }

        vacps::http::ScriptHttpRequest put;
        put.method = "PUT";
        put.path = "/schedulers/sched-test-1";
        put.headers.emplace_back("content-type", "application/json");
        put.body = R"({
          "cron": "0 0 1 1 *",
          "timezone": "UTC",
          "enabled": true,
          "next_run_at": "2099-01-01T00:00:00.000Z",
          "task": {
            "kind": "command",
            "backend_id": "local",
            "program": "/bin/true",
            "arguments": [],
            "timeout_seconds": 10,
            "working_directory": "/tmp",
            "profile": "full",
            "output": {
              "capture_stdout": true,
              "capture_stderr": true,
              "preview_max_bytes": 8192,
              "retention_seconds": 86400,
              "hard_max_bytes": 10485760
            }
          }
        })";
        auto pr = co_await vacps::http::dispatch_to_script(*host, std::move(put));
        if (!pr || (pr->status != 204 && pr->status != 200)) {
          err = pr ? ("put sched " + std::to_string(pr->status) + " " + pr->body)
                   : pr.error().message;
          co_return;
        }

        vacps::http::ScriptHttpRequest run;
        run.method = "POST";
        run.path = "/schedulers/sched-test-1/run";
        run.headers.emplace_back("content-type", "application/json");
        run.body = R"({
          "task": {
            "kind": "command",
            "backend_id": "local",
            "program": "/bin/echo",
            "arguments": ["sched-ok"],
            "timeout_seconds": 10,
            "working_directory": "/tmp",
            "profile": "full",
            "output": {
              "capture_stdout": true,
              "capture_stderr": true,
              "preview_max_bytes": 8192,
              "retention_seconds": 86400,
              "hard_max_bytes": 10485760
            }
          }
        })";
        auto rr = co_await vacps::http::dispatch_to_script(*host, std::move(run));
        if (!rr || rr->status != 200) {
          err = rr ? ("run " + rr->body) : rr.error().message;
          co_return;
        }
        if (rr->body.find("task_id") == std::string::npos) {
          err = "run body: " + rr->body;
          co_return;
        }

        auto tick = co_await host->invoke_export("tickControlPlane", 0, nullptr);
        if (!tick) {
          err = tick.error().message;
          co_return;
        }

        auto sh = co_await host->shutdown_script();
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
}

TEST_F(JsTasksTest, FsGlobEditPatch) {
  ASSERT_TRUE(fs::exists(business_script()));

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;
  const auto work = dir_.string();
  const auto file_path = (dir_ / "sample.txt").string();

  asio::co_spawn(
      ioc,
      [host, &ok, &err, script = business_script().string(), work, file_path]()
          -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        // write sample
        vacps::http::ScriptHttpRequest write;
        write.method = "POST";
        write.path = "/fs/write";
        write.headers.emplace_back("content-type", "application/json");
        write.body = std::string(R"({
          "path": ")") +
                     file_path + R"(",
          "content": "hello world\nfoo bar\n",
          "mode": "create_or_overwrite"
        })";
        auto wr = co_await vacps::http::dispatch_to_script(*host, std::move(write));
        if (!wr || wr->status != 200) {
          err = wr ? wr->body : wr.error().message;
          co_return;
        }

        // edit
        vacps::http::ScriptHttpRequest edit;
        edit.method = "POST";
        edit.path = "/fs/edit";
        edit.headers.emplace_back("content-type", "application/json");
        edit.body = std::string(R"({
          "path": ")") +
                    file_path + R"(",
          "old_text": "hello world",
          "new_text": "hello native"
        })";
        auto er = co_await vacps::http::dispatch_to_script(*host, std::move(edit));
        if (!er || er->status != 200) {
          err = er ? ("edit " + er->body) : er.error().message;
          co_return;
        }
        if (er->body.find("replacement_count") == std::string::npos) {
          err = "edit body: " + er->body;
          co_return;
        }

        // glob
        vacps::http::ScriptHttpRequest glob;
        glob.method = "POST";
        glob.path = "/fs/glob";
        glob.headers.emplace_back("content-type", "application/json");
        glob.body = std::string(R"({
          "pattern": "*.txt",
          "path": ")") +
                    work + R"("
        })";
        auto gr = co_await vacps::http::dispatch_to_script(*host, std::move(glob));
        if (!gr || gr->status != 200) {
          err = gr ? ("glob " + gr->body) : gr.error().message;
          co_return;
        }
        if (gr->body.find("sample.txt") == std::string::npos) {
          err = "glob missing file: " + gr->body;
          co_return;
        }

        // capabilities
        vacps::http::ScriptHttpRequest cap;
        cap.method = "GET";
        cap.path = "/capabilities";
        auto cr = co_await vacps::http::dispatch_to_script(*host, std::move(cap));
        if (!cr || cr->status != 200) {
          err = cr ? ("cap " + cr->body) : cr.error().message;
          co_return;
        }
        if (cr->body.find("\"pi\":false") == std::string::npos &&
            cr->body.find("\"pi\": false") == std::string::npos) {
          // accept either spacing from JSON.stringify
          if (cr->body.find("pi") == std::string::npos) {
            err = "capabilities: " + cr->body;
            co_return;
          }
        }

        // apply_patch add
        const auto added = (fs::path(work) / "patched.txt").string();
        vacps::http::ScriptHttpRequest patch;
        patch.method = "POST";
        patch.path = "/fs/apply_patch";
        patch.headers.emplace_back("content-type", "application/json");
        // relative path under workspace
        patch.body = std::string(R"({
          "workspace_path": ")") +
                     work + R"(",
          "patch": "*** Add File: patched.txt\n+patched-ok\n"
        })";
        auto pr = co_await vacps::http::dispatch_to_script(*host, std::move(patch));
        if (!pr || pr->status != 200) {
          err = pr ? ("patch " + pr->body) : pr.error().message;
          co_return;
        }
        if (pr->body.find("applied") == std::string::npos) {
          err = "patch body: " + pr->body;
          co_return;
        }
        (void)added;

        auto sh = co_await host->shutdown_script();
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
}

TEST_F(JsTasksTest, ProcessStartReadTerminate) {
  ASSERT_TRUE(fs::exists(business_script()));

  asio::io_context ioc{1};
  auto host_r = vacps::js::Host::create(cfg_, ioc);
  ASSERT_TRUE(host_r) << host_r.error().message;
  auto host = std::move(*host_r);

  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [host, &ok, &err, script = business_script().string()]() -> asio::awaitable<void> {
        auto init = co_await host->load_and_initialize(script);
        if (!init) {
          err = init.error().message;
          co_return;
        }

        vacps::http::ScriptHttpRequest start;
        start.method = "POST";
        start.path = "/process/start_command";
        start.headers.emplace_back("content-type", "application/json");
        start.body = R"({
          "program": "/bin/sh",
          "arguments": ["-c", "echo hi-start; sleep 5"],
          "timeout_ms": 30000,
          "working_directory": "/tmp"
        })";
        auto sr = co_await vacps::http::dispatch_to_script(*host, std::move(start));
        if (!sr || sr->status != 200) {
          err = sr ? ("start " + std::to_string(sr->status) + " " + sr->body)
                   : sr.error().message;
          co_return;
        }
        auto id_pos = sr->body.find("\"process_id\"");
        if (id_pos == std::string::npos) {
          err = "no process_id: " + sr->body;
          co_return;
        }
        auto colon = sr->body.find(':', id_pos);
        auto v1 = sr->body.find('"', colon + 1);
        auto v2 = sr->body.find('"', v1 + 1);
        if (v1 == std::string::npos || v2 == std::string::npos) {
          err = "process_id quotes: " + sr->body;
          co_return;
        }
        const std::string proc_id = sr->body.substr(v1 + 1, v2 - v1 - 1);

        vacps::http::ScriptHttpRequest read;
        read.method = "POST";
        read.path = "/process/read";
        read.headers.emplace_back("content-type", "application/json");
        read.body = std::string(R"({"process_id":")") + proc_id +
                    R"(","wait_ms":2000,"max_bytes":65536})";
        auto rr = co_await vacps::http::dispatch_to_script(*host, std::move(read));
        if (!rr || rr->status != 200) {
          err = rr ? ("read " + std::to_string(rr->status) + " " + rr->body)
                   : rr.error().message;
          co_return;
        }
        if (rr->body.find("hi-start") == std::string::npos) {
          err = "read missing stdout: " + rr->body;
          co_return;
        }

        vacps::http::ScriptHttpRequest term;
        term.method = "POST";
        term.path = "/process/terminate";
        term.headers.emplace_back("content-type", "application/json");
        term.body = std::string(R"({"process_id":")") + proc_id +
                    R"(","signal":"sigkill","grace_period_ms":0})";
        auto tr = co_await vacps::http::dispatch_to_script(*host, std::move(term));
        if (!tr || tr->status != 200) {
          err = tr ? ("term " + std::to_string(tr->status) + " " + tr->body)
                   : tr.error().message;
          co_return;
        }

        auto sh = co_await host->shutdown_script();
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
}
