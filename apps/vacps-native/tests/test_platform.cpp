#include "app/platform.hpp"

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <string_view>

TEST(PlatformTest, StringPresentAndWellFormed) {
  const std::string_view p = vacps::platform_string();
  ASSERT_FALSE(p.empty());
  // CMake VACPS_PLATFORM_STRING or preprocessor fallback (os-arch-libc).
  static const std::regex kRe{
      R"(^(linux|darwin|unknown)-(x86_64|aarch64|unknown)-(musl|gnu|unknown)$)"};
  EXPECT_TRUE(std::regex_match(std::string{p}, kRe)) << "platform=" << p;
}

TEST(PlatformTest, MatchesCompileDefinitionWhenSet) {
#ifdef VACPS_PLATFORM_STRING
  // Macro expands to a string literal; platform_string() returns the same.
  EXPECT_STREQ(vacps::platform_string(), VACPS_PLATFORM_STRING);
#endif
}
