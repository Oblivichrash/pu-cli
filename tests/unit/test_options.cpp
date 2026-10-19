// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/runtime/runtime.hpp"
#include "pu/app/cli.hpp"
#include "pu/core/json.hpp"
#include "pu/core/platform.hpp"

#include <boost/program_options.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace json = pu::json;
namespace po = boost::program_options;

// The parse cases that used to open this file built an options_description of their own and
// asserted what boost::program_options does with it. Nothing about `pu` was under test: the
// command line `pu` actually parses is the one BuildCommandLine assembles, and the case below
// is what pins it. A parsing regression in boost belongs to boost's own tests.
//
// A parsed option's effect is covered where it is acted on: test_cli_exit drives RunAsk,
// test_listen_options drives the serve options, and test_agent_config covers the file those
// options name.

TEST_CASE("The command line names every option once", "[options]") {
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();

  const auto parse = [&](std::vector<std::string> args) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argv.size()), argv.data())
                  .options(line.all)
                  .positional(line.positional)
                  .run(),
              vm);
    po::notify(vm);
    return vm;
  };

  SECTION("ask takes an agent and a prompt") {
    const po::variables_map vm =
        parse({"pu", "ask", "summarize the readme", "--agent", "test-agent"});
    REQUIRE(vm["command"].as<std::string>() == "ask");
    REQUIRE(vm["agent"].as<std::string>() == "test-agent");
    REQUIRE(vm["prompt"].as<std::string>() == "summarize the readme");
  }

  SECTION("chat takes an agent") {
    const po::variables_map vm = parse({"pu", "chat", "--agent", "test-agent"});
    REQUIRE(vm["command"].as<std::string>() == "chat");
    REQUIRE(vm["agent"].as<std::string>() == "test-agent");
  }

  SECTION("models takes an agent and a verify flag") {
    const po::variables_map plain = parse({"pu", "models", "--agent", "test-agent"});
    REQUIRE(plain["command"].as<std::string>() == "models");
    REQUIRE(plain["agent"].as<std::string>() == "test-agent");
    REQUIRE(plain.count("verify") == 0);

    const po::variables_map verifying = parse({"pu", "models", "--verify"});
    REQUIRE(verifying["command"].as<std::string>() == "models");
    REQUIRE(verifying.count("verify") == 1);
  }

  SECTION("serve takes a host and a port") {
    const po::variables_map vm = parse({"pu", "serve", "--host", "0.0.0.0", "--port", "9000"});
    REQUIRE(vm["host"].as<std::string>() == "0.0.0.0");
    REQUIRE(vm["port"].as<std::string>() == "9000");
  }
}
