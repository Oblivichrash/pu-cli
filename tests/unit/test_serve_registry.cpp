// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/serve_registry.hpp"
#include "pu/core/platform.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

using namespace pu;
using namespace pu::tests;

namespace fs = std::filesystem;

namespace {

config::ServeRecord Record(int pid, std::uint16_t port, const std::string& directory) {
  return config::ServeRecord{directory, "127.0.0.1", port, pid};
}

// The registry is written under the user's home, so a case that registers servers would
// otherwise edit the file of whoever is running the tests. This moves home for the length of
// the case and removes what was written there.
struct HomeScope {
  HomeScope() : home(UniqueTempPath("pu_test_home")), env("HOME", home.string()) {
    fs::create_directories(home);
  }
  ~HomeScope() {
    std::error_code ec;
    fs::remove_all(home, ec);
  }
  fs::path home;
  ScopedEnvVar env;
};

}  // namespace

TEST_CASE("a server registered from one directory is readable from another") {
  HomeScope scoped;
  const int pid = platform::CurrentProcessId();

  REQUIRE(config::ServeRegistryPath() == (scoped.home / ".pu" / "serve.json").string());
  REQUIRE_FALSE(config::RegisterServeRecord(Record(pid, 8811, "/work/one")).empty());

  const auto records = config::LiveServeRecords();
  REQUIRE(records.size() == 1);
  CHECK(records.front().directory == "/work/one");
  CHECK(records.front().host == "127.0.0.1");
  CHECK(records.front().pid == pid);

  config::UnregisterServeRecord(pid);
  CHECK(config::LiveServeRecords().empty());
}

TEST_CASE("a server whose process has stopped is dropped from the list") {
  HomeScope scoped;
  const int here = platform::CurrentProcessId();
  const std::string path = config::ServeRegistryPath();

  REQUIRE_FALSE(config::RegisterServeRecord(Record(here, 8812, "/work/here")).empty());
  REQUIRE_FALSE(config::RegisterServeRecord(Record(-1, 8813, "/work/gone")).empty());
  REQUIRE(fs::exists(path));

  const auto records = config::LiveServeRecords();
  REQUIRE(records.size() == 1);
  CHECK(records.front().port == 8812);
  // The line is deleted as well as hidden, so the next reader starts from a list that is true
  // rather than one that has to be filtered again.
  CHECK(fs::file_size(path) < 200);

  config::UnregisterServeRecord(here);
}

TEST_CASE("registering again replaces what this process said before") {
  HomeScope scoped;
  const int pid = platform::CurrentProcessId();

  REQUIRE_FALSE(config::RegisterServeRecord(Record(pid, 8814, "/work/before")).empty());
  REQUIRE_FALSE(config::RegisterServeRecord(Record(pid, 8815, "/work/after")).empty());

  const auto records = config::LiveServeRecords();
  REQUIRE(records.size() == 1);
  CHECK(records.front().port == 8815);
  CHECK(records.front().directory == "/work/after");

  config::UnregisterServeRecord(pid);
}

TEST_CASE("a process is running when this one is") {
  CHECK(platform::ProcessRunning(platform::CurrentProcessId()));
  CHECK_FALSE(platform::ProcessRunning(-1));
}
