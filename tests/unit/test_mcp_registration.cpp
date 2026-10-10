// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pu/core/json.hpp"
#include "pu/runtime.hpp"
#include "tests/mocks/serve_harness.hpp"
#include "tests/mocks/test_helpers.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;

using namespace pu::tests;

namespace {

http::response<http::string_body> Answer(unsigned status, std::string body) {
  http::response<http::string_body> res;
  res.result(static_cast<http::status>(status));
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
    if (!pu::json::HasKey(body, "id")) return Answer(200, "");

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
      reply["result"] = {{"content", boost::json::array{boost::json::value{
                                          {"type", "text"}, {"text", "served"}}}}};
    } else {
      reply["error"] = {{"code", -32601}, {"message", "no such method"}};
    }
    return Answer(200, boost::json::serialize(reply));
  }

 private:
  std::string tool_name_;
};

class ToolCapturingBackend {
 public:
  http::response<http::string_body> operator()(const http::request<http::string_body>& req) {
    const auto body = boost::json::parse(req.body());
    if (pu::json::HasKey(body, "tools") && body.at("tools").is_array()) {
      std::set<std::string> names;
      for (const auto& tool : body.at("tools").as_array()) {
        if (pu::json::HasKey(tool, "function")) {
          names.insert(boost::json::value_to<std::string>(tool.at("function").at("name")));
        } else if (pu::json::HasKey(tool, "name")) {
          names.insert(boost::json::value_to<std::string>(tool.at("name")));
        }
      }
      std::lock_guard<std::mutex> lock(mutex_);
      offered_ = std::move(names);
    }
    return Answer(200, R"({"message":{"content":"OK"}}
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

void WriteAgentWithMcpServer(const fs::path& dir, int backend_port, int mcp_port,
                             const std::string& server_name) {
  fs::create_directories(dir / ".pu");
  boost::json::value root = {
      {"default_agent", "chat"},
      {"agents",
       boost::json::array{boost::json::value{
           {"name", "chat"},
           {"description", "Chat agent"},
           {"backend",
            {{"type", "openai"},
             {"host", "http://127.0.0.1:" + std::to_string(backend_port)},
             {"model", "test-model"}}},
           {"security", {{"sandbox_root", "."}, {"forbidden_patterns", boost::json::array{}}}},
           {"mcp_servers",
            boost::json::array{boost::json::value{
                {"name", server_name},
                {"url", "http://127.0.0.1:" + std::to_string(mcp_port) + "/mcp"}}}}}}}};

  std::ofstream out(dir / ".pu" / "agents.json", std::ios::trunc);
  out << boost::json::serialize(root);
}

}  // namespace

TEST_CASE("A tool from a configured MCP server reaches the provider", "[mcp][runtime]") {
  ScopedTempDir workspace("pu_mcp_registration");
  ScopedWorkingDir in_workspace(workspace.Path());

  McpToolServer mcp("search");
  FakeHttpServer mcp_http([&](const http::request<http::string_body>& req) { return mcp(req); });

  ToolCapturingBackend backend;
  FakeHttpServer backend_http(
      [&](const http::request<http::string_body>& req) { return backend(req); });

  WriteAgentWithMcpServer(workspace.Path(), backend_http.Port(), mcp_http.Port(), "files");

  pu::Runtime runtime;
  runtime.Initialize();

  bool is_command = false;
  runtime.ProcessInput("hello", is_command);

  const std::set<std::string> offered = backend.Offered();
  REQUIRE(offered.count("mcp_files_search") == 1);
  REQUIRE(offered.count("execute_bash") == 1);

  runtime.Shutdown();
}

TEST_CASE("A rebuilt toolbox does not keep a tool from a server that is gone", "[mcp][runtime]") {
  ScopedTempDir workspace("pu_mcp_rebuild");
  ScopedWorkingDir in_workspace(workspace.Path());

  ToolCapturingBackend backend;
  FakeHttpServer backend_http(
      [&](const http::request<http::string_body>& req) { return backend(req); });

  int mcp_port = 0;
  {
    McpToolServer mcp("temporary");
    FakeHttpServer mcp_http([&](const http::request<http::string_body>& req) { return mcp(req); });
    mcp_port = mcp_http.Port();

    WriteAgentWithMcpServer(workspace.Path(), backend_http.Port(), mcp_port, "files");

    pu::Runtime runtime;
    runtime.Initialize();

    bool is_command = false;
    runtime.ProcessInput("hello", is_command);
    REQUIRE(backend.Offered().count("mcp_files_temporary") == 1);

    runtime.Shutdown();
  }

  WriteAgentWithMcpServer(workspace.Path(), backend_http.Port(), mcp_port, "files");

  pu::Runtime second;
  second.Initialize();

  bool is_command = false;
  second.ProcessInput("hello", is_command);

  const std::set<std::string> offered = backend.Offered();
  REQUIRE(offered.count("mcp_files_temporary") == 0);

  second.Shutdown();
}
