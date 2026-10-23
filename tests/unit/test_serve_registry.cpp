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

// One home per process, which is what the process already had before this case moved it. The
// cases run at the same time as separate processes, and a home shared with another process is a
// directory another process is free to delete while this one is registering into it — so a home
// is asked for once and every case in this process writes into that one.
fs::path ScopedHome() {
  static const fs::path home = [] {
    const fs::path path = UniqueTempPath("pu_test_home");
    fs::create_directories(path);
    return path;
  }();
  return home;
}

config::ServeRecord Record(int pid, std::uint16_t port, const std::string& directory) {
  // The stamp of this process, which is the only one a case can write honestly: a made-up stamp
  // would be dropped by the rule that a record is worth reading only while its own holder runs.
  return config::ServeRecord{directory, "127.0.0.1", port, pid,
                             platform::ProcessStartToken(pid)};
}

// The registry is written under the user's home, so a case that registers servers would
// otherwise edit the file of whoever is running the tests. This moves home for the length of
// the case and removes what was written there, so that a case leaves no record naming a pid and
// a start time from before in a temp directory that outlives the run that wrote it.
// ScopedEnvVar writes the process environment, not the C library's mirror of it, so the case
// also has to say that the mirror is what ServeRegistryPath() reads.
struct HomeScope {
  HomeScope()
      : home(ScopedHome()),
        env("HOME", home.string()),
        mirror{} {}
  ~HomeScope() {
    std::error_code ec;
    fs::remove(home / ".pu" / "serve.json", ec);
    fs::remove_all(home, ec);
  }
  fs::path home;
  ScopedEnvVar env;
  platform::LiveEnvironment mirror;
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
  CHECK(records.front().bind == "127.0.0.1");
  CHECK(records.front().pid == pid);
  CHECK(records.front().started == platform::ProcessStartToken(pid));

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

// The page names a workspace rather than a port, and it does so from a label the server
// computes. Two segments is the rule: the last one is the project, and the one before it is what
// tells two projects with the same name apart. The whole path never crosses the wire.
TEST_CASE("a workspace is named by its last two segments") {
  CHECK(config::WorkspaceLabel("/home/a/pu-cli") == "a/pu-cli");
  CHECK(config::WorkspaceLabel("/home/a/pu-cli/") == "a/pu-cli");
  CHECK(config::WorkspaceLabel("pu-cli") == "pu-cli");
  CHECK(config::WorkspaceLabel("/srv/work/one/two/three") == "two/three");
  CHECK(config::WorkspaceLabel("") == "");

  // A server started from a relative path serves a real directory under a name that says nothing
  // about it, so the label resolves the markers before it counts segments: without that, `..`
  // would be the parent's name and `.` would be the whole label.
  CHECK(config::WorkspaceLabel("/home/a/pu-cli/.") == "a/pu-cli");
  CHECK(config::WorkspaceLabel("/home/a/pu-cli/../other") == "a/other");
  CHECK(config::WorkspaceLabel("/home/a/work/./pu-cli") == "work/pu-cli");
}

// The list a page draws is what it may switch to, so the server that answered is not in it: it is
// the page's own. Filtering by pid and not by directory is what makes that true when two servers
// serve the same workspace, which is legal and would otherwise list a switch to itself.
TEST_CASE("the list of other servers leaves this process's own record out") {
  const std::vector<config::ServeRecord> all{
      {"/work/one", "127.0.0.1", 8811, 100},
      {"/work/two", "127.0.0.1", 8812, 200},
      {"/work/three", "0.0.0.0", 8813, 300},
  };

  const auto others = config::OthersAmong(all, 200);
  REQUIRE(others.size() == 2);
  CHECK(others[0].directory == "/work/one");
  CHECK(others[1].directory == "/work/three");

  CHECK(config::OthersAmong(all, 999).size() == 3);
  CHECK(config::OthersAmong({}, 100).empty());
}

// A pid is unique only among the processes running now. Once its owner exits the number is
// handed to the next program, so a record that stored a pid and asked later whether "the
// process" was alive would get a true answer about something else and the page would list a
// server nobody can reach. The stamp is the discriminator, and it is the one a record has to
// carry.
TEST_CASE("a record whose pid now belongs to another program is not a running server") {
  HomeScope scoped;
  const int pid = platform::CurrentProcessId();
  const std::string real = platform::ProcessStartToken(pid);

  config::ServeRecord record = Record(pid, 8816, "/work/one");
  REQUIRE(record.started == real);

  // Only meaningful where the platform reports a start time; where it does not, the pid is all
  // there is to go on and the record is kept, which is the answer given before stamps existed.
  if (!real.empty()) {
    config::ServeRecord stale = record;
    stale.started = real + "0";
    CHECK_FALSE(config::StillRunning(stale));

    config::ServeRecord unstamped = record;
    unstamped.started.clear();
    CHECK(config::StillRunning(unstamped));
  }

  CHECK(config::StillRunning(record));

  // A pid no process holds is not running whatever the stamp says.
  config::ServeRecord gone = Record(-1, 8817, "/work/gone");
  CHECK_FALSE(config::StillRunning(gone));
}

TEST_CASE("a process is running when this one is") {
  CHECK(platform::ProcessRunning(platform::CurrentProcessId()));
  CHECK_FALSE(platform::ProcessRunning(-1));
}
