// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "pu/config/agents.hpp"

using pu::config::ParsePort;
using pu::config::ResolveListenOptions;
using pu::config::ServeOptions;

TEST_CASE("A port is the number a text names, or nothing", "[listen]") {
  REQUIRE(ParsePort("8080") == 8080);
  REQUIRE(ParsePort("1") == 1);
  REQUIRE(ParsePort("65535") == 65535);

  // Not a port: nothing to listen on.
  REQUIRE_FALSE(ParsePort("").has_value());
  REQUIRE_FALSE(ParsePort("0").has_value());
  REQUIRE_FALSE(ParsePort("65536").has_value());
  REQUIRE_FALSE(ParsePort("70000").has_value());
  REQUIRE_FALSE(ParsePort("8080x").has_value());
  REQUIRE_FALSE(ParsePort("+80").has_value());
  REQUIRE_FALSE(ParsePort("-1").has_value());
  REQUIRE_FALSE(ParsePort(" 8080").has_value());
  REQUIRE_FALSE(ParsePort("8 0").has_value());
  REQUIRE_FALSE(ParsePort("8080 ").has_value());
  REQUIRE_FALSE(ParsePort("123456789").has_value());
}

TEST_CASE("Where to listen: the command line, then the environment, then the file", "[listen]") {
  const ServeOptions file{"from-file", 9000};

  const auto plain =
      ResolveListenOptions(std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(plain.host == "127.0.0.1");
  REQUIRE(plain.port == 8080);

  const auto from_file =
      ResolveListenOptions(std::nullopt, std::nullopt, std::nullopt, std::nullopt, file);
  REQUIRE(from_file.host == "from-file");
  REQUIRE(from_file.port == 9000);

  const auto from_env = ResolveListenOptions(std::nullopt, std::nullopt, "env-host", "9100", file);
  REQUIRE(from_env.host == "env-host");
  REQUIRE(from_env.port == 9100);

  const auto from_flag = ResolveListenOptions("flag-host", "9200", "env-host", "9100", file);
  REQUIRE(from_flag.host == "flag-host");
  REQUIRE(from_flag.port == 9200);

  // Each place may answer for one of the two, and the other comes from the place below it.
  const auto mixed = ResolveListenOptions("flag-host", std::nullopt, std::nullopt, "9100", file);
  REQUIRE(mixed.host == "flag-host");
  REQUIRE(mixed.port == 9100);
}

// A port a shell named is refused rather than answered by a file: what was asked for is not
// silently replaced by what a directory happens to say.
TEST_CASE("A port that was named is not overruled by the file", "[listen]") {
  const ServeOptions file{"from-file", 9000};

  const auto refused_flag =
      ResolveListenOptions(std::nullopt, "70000", std::nullopt, std::nullopt, file);
  REQUIRE(refused_flag.port == 8080);
  REQUIRE(refused_flag.host == "from-file");

  const auto refused_env =
      ResolveListenOptions(std::nullopt, std::nullopt, std::nullopt, "not a port", file);
  REQUIRE(refused_env.port == 8080);
  REQUIRE(refused_env.host == "from-file");
}
