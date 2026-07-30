#include "app/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

void set_env(const char* k, const char* v) {
  if (v == nullptr) {
    unsetenv(k);
  } else {
    setenv(k, v, 1);
  }
}

}  // namespace

TEST(ConfigTest, Defaults) {
  set_env("VACPS_LISTEN_HOST", nullptr);
  set_env("VACPS_LISTEN_PORT", nullptr);
  set_env("VACPS_DATA_DIR", nullptr);
  set_env("VACPS_LOG_LEVEL", nullptr);
  set_env("VACPS_ALLOW_REMOTE_BIND", nullptr);

  const auto c = vacps::Config::from_env();
  EXPECT_EQ(c.listen_host, "127.0.0.1");
  EXPECT_EQ(c.listen_port, 8788);
  EXPECT_EQ(c.data_dir, "data");
  EXPECT_EQ(c.database_path(), "data/agent.db");
}

TEST(ConfigTest, EnvOverridesAndRemoteBindFailClosed) {
  set_env("VACPS_LISTEN_HOST", "0.0.0.0");
  set_env("VACPS_LISTEN_PORT", "9999");
  set_env("VACPS_DATA_DIR", "/tmp/vacps-x");
  set_env("VACPS_LOG_LEVEL", "debug");
  set_env("VACPS_ALLOW_REMOTE_BIND", "false");

  const auto c = vacps::Config::from_env();
  EXPECT_EQ(c.listen_host, "127.0.0.1");  // fail-closed
  EXPECT_EQ(c.listen_port, 9999);
  EXPECT_EQ(c.data_dir, "/tmp/vacps-x");
  EXPECT_EQ(c.log_level, "debug");
  EXPECT_EQ(c.database_path(), "/tmp/vacps-x/agent.db");

  set_env("VACPS_ALLOW_REMOTE_BIND", "true");
  set_env("VACPS_LISTEN_HOST", "0.0.0.0");
  const auto c2 = vacps::Config::from_env();
  EXPECT_EQ(c2.listen_host, "0.0.0.0");

  set_env("VACPS_LISTEN_HOST", nullptr);
  set_env("VACPS_LISTEN_PORT", nullptr);
  set_env("VACPS_DATA_DIR", nullptr);
  set_env("VACPS_LOG_LEVEL", nullptr);
  set_env("VACPS_ALLOW_REMOTE_BIND", nullptr);
}

TEST(ConfigTest, ParsePortBounds) {
  auto ok = vacps::parse_port("8788");
  ASSERT_TRUE(ok);
  EXPECT_EQ(*ok, 8788);

  EXPECT_FALSE(vacps::parse_port(""));
  EXPECT_FALSE(vacps::parse_port("0"));
  EXPECT_FALSE(vacps::parse_port("65536"));
  EXPECT_FALSE(vacps::parse_port("-1"));
  EXPECT_FALSE(vacps::parse_port("abc"));
  EXPECT_FALSE(vacps::parse_port("80x"));
  EXPECT_FALSE(vacps::parse_port(" 80"));
  auto max = vacps::parse_port("65535");
  ASSERT_TRUE(max);
  EXPECT_EQ(*max, 65535);
  auto one = vacps::parse_port("1");
  ASSERT_TRUE(one);
  EXPECT_EQ(*one, 1);
}

TEST(ConfigTest, CliHostFailClosedViaPolicy) {
  set_env("VACPS_ALLOW_REMOTE_BIND", nullptr);
  vacps::Config c;
  c.listen_host = "0.0.0.0";  // as if CLI set it
  vacps::apply_remote_bind_policy(c);
  EXPECT_EQ(c.listen_host, "127.0.0.1");

  set_env("VACPS_ALLOW_REMOTE_BIND", "true");
  c.listen_host = "0.0.0.0";
  vacps::apply_remote_bind_policy(c);
  EXPECT_EQ(c.listen_host, "0.0.0.0");

  set_env("VACPS_ALLOW_REMOTE_BIND", nullptr);
  EXPECT_TRUE(vacps::is_loopback_host("127.0.0.1"));
  EXPECT_TRUE(vacps::is_loopback_host("localhost"));
  EXPECT_TRUE(vacps::is_loopback_host("::1"));
  EXPECT_FALSE(vacps::is_loopback_host("0.0.0.0"));
}

TEST(ConfigTest, InvalidEnvPortFallsBack) {
  set_env("VACPS_LISTEN_PORT", "not-a-port");
  set_env("VACPS_ALLOW_REMOTE_BIND", nullptr);
  set_env("VACPS_LISTEN_HOST", nullptr);
  const auto c = vacps::Config::from_env();
  EXPECT_EQ(c.listen_port, 8788);
  set_env("VACPS_LISTEN_PORT", nullptr);
}
