// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/runtime.hpp"
#include "pu/cli.hpp"
#include "pu/json.hpp"
#include "pu/infra/platform.hpp"

#include <boost/program_options.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace json = pu::json;
namespace po = boost::program_options;

namespace {

class ScopedEnvVar {
 public:
  ScopedEnvVar(const std::string& name, const std::string& value) : name_(name) {
    const char* prev = std::getenv(name.c_str());
    had_prev_ = (prev != nullptr);
    if (had_prev_) prev_ = prev;
    Set(value);
  }

  ~ScopedEnvVar() {
    if (had_prev_) Set(prev_);
    else Unset();
  }

 private:
  void Set(const std::string& value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }

  void Unset() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), "");
#else
    unsetenv(name_.c_str());
#endif
  }

  std::string name_;
  std::string prev_;
  bool had_prev_ = false;
};

}  // namespace

TEST_CASE("Boost.Program_options parses ask command", "[boost][program_options]") {
  const char* argv[] = {"pu", "ask", "--agent", "test-agent", "hello world"};
  int argc = 5;

  po::options_description desc;
  desc.add_options()
    ("help,h", "help")
    ("command", po::value<std::string>(), "command")
    ("agent", po::value<std::string>(), "agent")
    ("prompt", po::value<std::string>(), "prompt");

  po::positional_options_description pos;
  pos.add("command", 1);
  pos.add("prompt", 1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
            .options(desc)
            .positional(pos)
            .run(), vm);
  po::notify(vm);

  REQUIRE(vm.count("command"));
  REQUIRE(vm["command"].as<std::string>() == "ask");
  REQUIRE(vm.count("agent"));
  REQUIRE(vm["agent"].as<std::string>() == "test-agent");
  REQUIRE(vm.count("prompt"));
  REQUIRE(vm["prompt"].as<std::string>() == "hello world");
}

TEST_CASE("Boost.Program_options parses chat command", "[boost][program_options]") {
  const char* argv[] = {"pu", "chat", "--agent", "coding-agent"};
  int argc = 4;

  po::options_description desc;
  desc.add_options()
    ("command", po::value<std::string>(), "command")
    ("agent", po::value<std::string>(), "agent");

  po::positional_options_description pos;
  pos.add("command", 1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
            .options(desc)
            .positional(pos)
            .run(), vm);
  po::notify(vm);

  REQUIRE(vm.count("command"));
  REQUIRE(vm["command"].as<std::string>() == "chat");
  REQUIRE(vm.count("agent"));
  REQUIRE(vm["agent"].as<std::string>() == "coding-agent");
}

TEST_CASE("Boost.Program_options parses serve command", "[boost][program_options]") {
  const char* argv[] = {"pu", "serve", "--host", "0.0.0.0", "--port", "9000"};
  int argc = 6;

  po::options_description desc;
  desc.add_options()
    ("command", po::value<std::string>(), "command")
    ("host", po::value<std::string>(), "host")
    ("port", po::value<int>(), "port");

  po::positional_options_description pos;
  pos.add("command", 1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
            .options(desc)
            .positional(pos)
            .run(), vm);
  po::notify(vm);

  REQUIRE(vm.count("command"));
  REQUIRE(vm["command"].as<std::string>() == "serve");
  REQUIRE(vm.count("host"));
  REQUIRE(vm["host"].as<std::string>() == "0.0.0.0");
  REQUIRE(vm.count("port"));
  REQUIRE(vm["port"].as<int>() == 9000);
}

TEST_CASE("Boost.Program_options shows help with -h", "[boost][program_options]") {
  const char* argv[] = {"pu", "ask", "-h"};
  int argc = 3;

  po::options_description desc("Usage");
  desc.add_options()
    ("help,h", "help")
    ("command", po::value<std::string>(), "command");

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
            .options(desc)
            .run(), vm);
  po::notify(vm);

  REQUIRE(vm.count("help") == 1);
}

TEST_CASE("Boost.Program_options rejects unknown command", "[boost][program_options]") {
  const char* argv[] = {"pu", "unknown"};
  int argc = 2;

  po::options_description desc;
  desc.add_options()
    ("command", po::value<std::string>(), "command");

  po::positional_options_description pos;
  pos.add("command", 1);

  po::variables_map vm;
  REQUIRE_NOTHROW(po::store(po::command_line_parser(argc, argv)
                            .options(desc)
                            .positional(pos)
                            .run(), vm));
  po::notify(vm);

  REQUIRE(vm.count("command"));
  REQUIRE(vm["command"].as<std::string>() == "unknown");
}
