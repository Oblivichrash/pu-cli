// SPDX-License-Identifier: GPL-3.0-only

//
// The exit status of a run, checked where a caller reads it: `RunAsk` returns a number, so
// these cases drive it and look at the number. The contract is docs/design/
// cli-agent-workflow.md §3.1 — a script's remedy depends on which of these it got.
//
// The harness owns a temp workspace and a fake backend on a port of its own, so a case
// either points the turn at that backend (answered) or at nothing (unreachable).
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "pu/app/cli.hpp"
#include "pu/config/agents.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"
#include "tests/mocks/serve_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Points the agent's backend at `port`, leaving everything else the harness wrote alone.
// Written with the harness's own writer so the file stays one the runtime can read.
void RepointBackend(const fs::path& home, int port) {
  pu::tests::WriteAgentsFile(home, port, "ollama");
}

// A port nothing listens on: the turn fails in the transport, which is the provider case.
constexpr int kClosedPort = 1;

}  // namespace

TEST_CASE("A turn that answered exits 0", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  REQUIRE(pu::cli::RunAsk("chat", "hello", harness.RuntimeRef()) == pu::cli::kOk);
}

TEST_CASE("A provider that could not be reached exits 2", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  // The harness's runtime is already initialized against its fake backend. `RunAsk` builds
  // its own runtime for a one-shot run, so the config is what carries the unreachable port.
  RepointBackend(harness.Home(), kClosedPort);

  pu::Runtime one_shot;
  REQUIRE(pu::cli::RunAsk("chat", "hello", one_shot) == pu::cli::kProvider);

  // The failed turn is not the mistake in the invocation, so it is not kUsage.
  REQUIRE(pu::cli::kProvider != pu::cli::kUsage);
}

TEST_CASE("A command the router answered is not a provider failure", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  pu::Runtime one_shot;
  // `/help` is answered by the command router, so nothing was asked of a provider. It did
  // what it was asked, which is 0 even though no reply was drawn.
  const int status = pu::cli::RunAsk("chat", "/help", one_shot);
  REQUIRE(status == pu::cli::kOk);
}
