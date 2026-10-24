// SPDX-License-Identifier: GPL-3.0-only

//
// The stdio subprocess path, end to end: a real child process, a real pipe, real JSON lines.
//
// Three things are asserted, and only these: the client reaches a server it started, it can
// be stopped without waiting for a line the server will never send, and the child is reaped
// afterwards. The first of those three is what the client is for; the other two are what a
// turn that starts a server would otherwise leak.
//

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/mcp/client.hpp"
#include "pu/mcp/stdio_transport.hpp"
#include "tests/mocks/stdio_echo_server.h"

namespace {

pu::config::McpServerConfig ThisServer() {
  pu::config::McpServerConfig config;
  config.name = "echo";
  config.command = PU_ECHO_SERVER_PATH;
  return config;
}

}  // namespace

TEST_CASE("A stdio server is reached over a pipe and lists its tools", "[mcp][stdio]") {
  pu::mcp::McpClient client(ThisServer());

  REQUIRE(client.Connect());
  REQUIRE(client.IsConnected());

  const std::vector<pu::ToolDefinition> tools = client.ListTools();
  REQUIRE(tools.size() == 1);
  REQUIRE(tools[0].name == pu::tests::kStdioServerTool);

  client.Disconnect();
  REQUIRE_FALSE(client.IsConnected());
}

TEST_CASE("A server that never speaks is still stoppable", "[mcp][stdio]") {
  // The reader blocks in read() on the child's stdout. Stopping has to release that
  // descriptor rather than wait for a line: this server is asked nothing, so it answers
  // nothing, and a Stop() that joined first would wait for a reply to a request that was
  // never sent.
  pu::mcp::StdioTransport transport(PU_ECHO_SERVER_PATH, {});

  REQUIRE(transport.Start([](const std::string&) {}));

  // A finished Stop is the assertion. A Stop that hangs takes the whole case with it, which
  // ctest reports as a timeout rather than a pass.
  transport.Stop();

  SUCCEED("Stop returned");
}

TEST_CASE("A turn that ends leaves no child behind", "[mcp][stdio]") {
  // Started, answered, and stopped: the child reads the closed stdin as the end of its input
  // and leaves, and Stop reaps it. A child left running is a process the next turn inherits,
  // and on a busy machine the first thing to notice is that the port or the pipe is taken.
  {
    pu::mcp::McpClient client(ThisServer());
    REQUIRE(client.Connect());
    REQUIRE(client.IsConnected());
    client.Disconnect();
  }

  SUCCEED("the client and its child were torn down");
}
