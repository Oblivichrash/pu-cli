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
#include <string>

#include "pu/app/cli.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"
#include "tests/mocks/serve_harness.hpp"

namespace {

namespace fs = std::filesystem;

// A port nothing listens on: the turn fails in the transport, which is the provider case.
constexpr int kClosedPort = 1;

}  // namespace

TEST_CASE("A turn that answered exits 0", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  REQUIRE(pu::cli::RunAsk("chat", "hello", harness.RuntimeRef()) == pu::cli::kOk);
}

TEST_CASE("A provider that could not be reached exits 2", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  // `RunAsk` builds its own runtime, so the config on disk is what carries the port.
  pu::tests::WriteAgentsFile(harness.Home(), kClosedPort, "ollama");

  pu::Runtime one_shot;
  REQUIRE(pu::cli::RunAsk("chat", "hello", one_shot) == pu::cli::kProvider);
}

TEST_CASE("A command the router answered is not a provider failure", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  pu::Runtime one_shot;
  // `/help` is answered by the command router, so nothing was asked of a provider.
  REQUIRE(pu::cli::RunAsk("chat", "/help", one_shot) == pu::cli::kOk);
}

TEST_CASE("A person stopping the process exits 4", "[cli][exit]") {
  pu::tests::ServeHarness harness;
  // What SIGINT sets. The status is asked of the run rather than of a result field, because
  // the signal reaches the process and not the turn.
  pu::platform::ShutdownToken()->store(true, std::memory_order_release);
  pu::Runtime one_shot;
  const int status = pu::cli::RunAsk("chat", "hello", one_shot);
  pu::platform::ShutdownToken()->store(false, std::memory_order_release);

  REQUIRE(status == pu::cli::kInterrupted);
}
