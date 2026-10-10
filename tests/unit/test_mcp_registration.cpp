// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <mutex>
#include <set>
#include <string>

#include "pu/core/json.hpp"
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

class ToolCapture {
 public:
  http::response<http::string_body> operator()(const http::request<http::string_body>& req) {
    const auto body = boost::json::parse(req.body());
    if (pu::json::HasKey(body, "tools") && body.at("tools").is_array()) {
      std::set<std::string> names;
      for (const auto& tool : body.at("tools").as_array()) {
        const boost::json::value& named =
            pu::json::HasKey(tool, "function") ? tool.at("function") : tool;
        if (pu::json::HasKey(named, "name")) {
          names.insert(boost::json::value_to<std::string>(named.at("name")));
        }
      }
      std::lock_guard<std::mutex> lock(mutex_);
      offered_ = std::move(names);
    }
    return Answer(R"({"message":{"content":"OK"}}
{"done":true}
)");
  }

  std::set<std::string> Offered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return offered_;
  }

 private:
  mutable std::mutex mutex_;
  std::set<std::string> offered_;
};

std::string McpUrl(int port) { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

}  // namespace

TEST_CASE("A tool from a configured MCP server reaches the provider", "[mcp][runtime]") {
  McpToolServer mcp("search");
  FakeHttpServer mcp_http(mcp.ToResponder());

  ToolCapture capture;
  ServeHarness harness("openai", 0, ServeHarness::McpServer{"files", McpUrl(mcp_http.Port())},
                       [&](const http::request<http::string_body>& req) { return capture(req); });

  bool is_command = false;
  harness.Runtime().ProcessInput("hello", is_command);

  const std::set<std::string> offered = capture.Offered();
  REQUIRE(offered.count("mcp_files_search") == 1);
  REQUIRE(offered.count("execute_bash") == 1);
}

TEST_CASE("A rebuilt toolbox does not keep a tool from a server that is gone", "[mcp][runtime]") {
  ToolCapture capture;
  int mcp_port = 0;

  {
    McpToolServer mcp("temporary");
    FakeHttpServer mcp_http(mcp.ToResponder());
    mcp_port = mcp_http.Port();

    ServeHarness harness("openai", 0, ServeHarness::McpServer{"files", McpUrl(mcp_port)},
                         [&](const http::request<http::string_body>& req) { return capture(req); });

    bool is_command = false;
    harness.Runtime().ProcessInput("hello", is_command);
    REQUIRE(capture.Offered().count("mcp_files_temporary") == 1);
  }

  ServeHarness harness("openai", 0, ServeHarness::McpServer{"files", McpUrl(mcp_port)},
                       [&](const http::request<http::string_body>& req) { return capture(req); });

  bool is_command = false;
  harness.Runtime().ProcessInput("hello", is_command);

  REQUIRE(capture.Offered().count("mcp_files_temporary") == 0);
}
