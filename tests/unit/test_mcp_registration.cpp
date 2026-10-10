// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <string>

#include "pu/core/json.hpp"
#include "pu/tools/toolbox.hpp"
#include "tests/mocks/serve_harness.hpp"

namespace beast = boost::beast;
namespace http = beast::http;

using namespace pu::tests;

namespace {

http::response<http::string_body> Answer(std::string body) {
  http::response<http::string_body> res;
  res.result(http::status::ok);
  res.set(http::field::content_type, "application/json");
  res.body() = std::move(body);
  res.prepare_payload();
  return res;
}

class McpToolServer {
 public:
  explicit McpToolServer(std::string tool_name) : tool_name_(std::move(tool_name)) {}

  http::response<http::string_body> operator()(const http::request<http::string_body>& req) {
    const auto body = boost::json::parse(req.body());
    if (!pu::json::HasKey(body, "id")) return Answer("");

    const int id = boost::json::value_to<int>(body.at("id"));
    const std::string method = boost::json::value_to<std::string>(body.at("method"));

    boost::json::object reply = {{"jsonrpc", "2.0"}, {"id", id}};
    if (method == "initialize") {
      reply["result"] = {{"protocolVersion", "2024-11-05"}};
    } else if (method == "tools/list") {
      reply["result"] = {{"tools", boost::json::array{boost::json::value{
                                      {"name", tool_name_},
                                      {"description", "A tool served over MCP"},
                                      {"inputSchema", {{"type", "object"}}}}}}};
    } else if (method == "tools/call") {
      reply["result"] = {
          {"content", boost::json::array{boost::json::value{{"type", "text"}, {"text", "served"}}}}};
    } else {
      reply["error"] = {{"code", -32601}, {"message", "no such method"}};
    }
    return Answer(boost::json::serialize(reply));
  }

  FakeHttpServer::Responder ToResponder() {
    return [this](const http::request<http::string_body>& req) { return (*this)(req); };
  }

 private:
  std::string tool_name_;
};

bool Offers(const pu::Toolbox& toolbox, const std::string& name) {
  const auto names = toolbox.Names();
  return std::find(names.begin(), names.end(), name) != names.end();
}

std::string McpUrl(int port) { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

}  // namespace

TEST_CASE("A tool from a configured MCP server reaches the toolbox", "[mcp][runtime]") {
  McpToolServer mcp("search");
  FakeHttpServer mcp_http(mcp.ToResponder());

  ServeHarness harness("ollama", 0, ServeHarness::McpServer{"files", McpUrl(mcp_http.Port())});

  // The dotted name the server reports is sanitized into a provider-safe identifier.
  REQUIRE(Offers(harness.Runtime().GetToolbox(), "mcp_files_search"));
  REQUIRE(Offers(harness.Runtime().GetToolbox(), "execute_bash"));
}

TEST_CASE("A rebuilt toolbox does not keep a tool from a server that is gone", "[mcp][runtime]") {
  int mcp_port = 0;
  {
    McpToolServer mcp("temporary");
    FakeHttpServer mcp_http(mcp.ToResponder());
    mcp_port = mcp_http.Port();

    ServeHarness harness("ollama", 0, ServeHarness::McpServer{"files", McpUrl(mcp_port)});
    REQUIRE(Offers(harness.Runtime().GetToolbox(), "mcp_files_temporary"));
  }

  ServeHarness harness("ollama", 0, ServeHarness::McpServer{"files", McpUrl(mcp_port)});
  REQUIRE_FALSE(Offers(harness.Runtime().GetToolbox(), "mcp_files_temporary"));
}
