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
  REQUIRE(plain.bind == "127.0.0.1");
  REQUIRE(plain.port == 8080);

  const auto from_file =
      ResolveListenOptions(std::nullopt, std::nullopt, std::nullopt, std::nullopt, file);
  REQUIRE(from_file.bind == "from-file");
  REQUIRE(from_file.port == 9000);

  const auto from_env = ResolveListenOptions(std::nullopt, std::nullopt, "env-bind", "9100", file);
  REQUIRE(from_env.bind == "env-bind");
  REQUIRE(from_env.port == 9100);

  const auto from_flag = ResolveListenOptions("flag-bind", "9200", "env-bind", "9100", file);
  REQUIRE(from_flag.bind == "flag-bind");
  REQUIRE(from_flag.port == 9200);

  const auto mixed = ResolveListenOptions("flag-bind", std::nullopt, std::nullopt, "9100", file);
  REQUIRE(mixed.bind == "flag-bind");
  REQUIRE(mixed.port == 9100);
}

TEST_CASE("A port that was named is not overruled by the file", "[listen]") {
  const ServeOptions file{"from-file", 9000};

  const auto refused_flag =
      ResolveListenOptions(std::nullopt, "70000", std::nullopt, std::nullopt, file);
  REQUIRE(refused_flag.port == 8080);
  REQUIRE(refused_flag.bind == "from-file");

  const auto refused_env =
      ResolveListenOptions(std::nullopt, std::nullopt, std::nullopt, "not a port", file);
  REQUIRE(refused_env.port == 8080);
  REQUIRE(refused_env.bind == "from-file");
}
