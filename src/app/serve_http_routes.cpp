// SPDX-License-Identifier: GPL-3.0-only
#include "serve_internal.hpp"

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/core/json.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

namespace pu::cli::detail {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;

void SendJson(http::response<http::string_body>& res, unsigned status,
              const boost::json::value& jv) {
  res.result(status);
  res.set(http::field::content_type, "application/json");
  res.body() = boost::json::serialize(jv);
  res.prepare_payload();
}

std::string GetWebDir() {
  static std::string dir;
  static std::once_flag flag;
  std::call_once(flag, []() {
    const char* env = std::getenv("PU_WEB_DIR");
    if (env && *env) {
      dir = env;
      return;
    }
    std::vector<std::string> candidates = {
      "./web",
      "../share/pu/web",
      "/usr/share/pu/web",
      "/usr/local/share/pu/web",
    };
    for (const auto& d : candidates) {
      if (std::filesystem::exists(d) && std::filesystem::is_directory(d)) {
        dir = d;
        return;
      }
    }
    dir = "./web";
  });
  return dir;
}

void ServeFile(const std::string& target, http::response<http::string_body>& res) {
  std::string base = GetWebDir();
  std::string path = base + target;
  if (target == "/")
    path = base + "/index.html";

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    res.result(http::status::not_found);
    res.prepare_payload();
    return;
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  res.result(http::status::ok);
  res.body() = std::move(content);
  if (path.ends_with(".html"))
    res.set(http::field::content_type, "text/html");
  else if (path.ends_with(".css"))
    res.set(http::field::content_type, "text/css");
  else if (path.ends_with(".js"))
    res.set(http::field::content_type, "application/javascript");
  else
    res.set(http::field::content_type, "application/octet-stream");
  res.prepare_payload();
}

void HandleApiSession(Runtime& runtime, std::mutex& io_mutex,
                      http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetDefaultSession();
    jv.as_object()["ok"] = session != nullptr;
    if (session) {
      const auto& spec = session->GetRuntimeSpec();
      jv.as_object()["agent_name"] = spec.agent_name;
      jv.as_object()["backend_type"] =
          spec.backend.type == config::BackendType::kOpenAI ? "openai" : "ollama";
      jv.as_object()["backend_model"] = spec.backend.model;
      jv.as_object()["backend_host"] = spec.backend.host;
    } else {
      jv.as_object()["error"] = "No active session";
    }
  } catch (const std::exception& e) {
    jv.as_object()["ok"] = false;
    jv.as_object()["error"] = e.what();
  }
  SendJson(res, 200, jv);
}

void HandleApiHistory(Runtime& runtime, std::mutex& io_mutex,
                      http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::array{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetDefaultSession();
    if (session) {
      auto history = session->GetWorkspace().GetHistory();
      for (const auto& msg : history) {
        boost::json::value item = {
          {"id", msg.id},
          {"role", msg.role},
          {"content", msg.content},
          {"timestamp", msg.timestamp},
        };
        if (msg.HasToolCalls())
          item.as_object()["tool_calls"] = msg.tool_calls;
        if (!msg.tool_call_id.empty())
          item.as_object()["tool_call_id"] = msg.tool_call_id;
        if (!msg.tool_name.empty())
          item.as_object()["tool_name"] = msg.tool_name;
        if (!msg.reasoning_content.empty())
          item.as_object()["reasoning_content"] = msg.reasoning_content;
        jv.as_array().push_back(item);
      }
    }
  } catch (const std::exception& e) {
    jv = boost::json::object{{"ok", false}, {"error", e.what()}};
    SendJson(res, 500, jv);
    return;
  }
  SendJson(res, 200, jv);
}

void HandleApiAgents(Runtime& runtime, std::mutex& io_mutex,
                     http::request<http::string_body>&&,
                     http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
  boost::json::array agents;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto& mgr = runtime.GetAgentManager();
    auto names = mgr.GetAgentNames();
    for (const auto& name : names) {
      auto* cfg = mgr.GetAgentConfig(name);
      boost::json::value item = {
        {"name", name},
        {"description", cfg ? cfg->description : ""},
      };
      agents.push_back(item);
    }
  } catch (const std::exception& e) {
    SendJson(res, 500, boost::json::object{{"success", false}, {"error", e.what()}});
    return;
  }
  jv.as_object()["agents"] = agents;
  SendJson(res, 200, jv);
}

void HandleApiAgentSwitch(Runtime& runtime, std::mutex& io_mutex,
                          http::request<http::string_body>&& req,
                          http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (const std::exception&) {
    boost::json::value err = {{"success", false}, {"error", "Invalid JSON"}};
    SendJson(res, 400, err);
    return;
  }
  if (!json::HasKey(body, "agent_name") || !body.at("agent_name").is_string()) {
    boost::json::value err = {{"success", false}, {"error", "Missing or invalid 'agent_name'"}};
    SendJson(res, 400, err);
    return;
  }
  std::string agent_name = boost::json::value_to<std::string>(body.at("agent_name"));
  boost::json::value resp = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto& mgr = runtime.GetAgentManager();
    auto* cfg = mgr.GetAgentConfig(agent_name);
    if (!cfg) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Agent not found: " + agent_name;
      SendJson(res, 404, resp);
      return;
    }
    runtime.SwitchAgent(*cfg);
    resp.as_object()["success"] = true;
    resp.as_object()["agent"] = agent_name;
  } catch (const std::exception& e) {
    resp.as_object()["success"] = false;
    resp.as_object()["error"] = e.what();
  }
  SendJson(res, 200, resp);
}

void HandleApiClear(Runtime& runtime, std::mutex& io_mutex,
                    http::request<http::string_body>&&,
                    http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetDefaultSession();
    if (session) {
      session->GetWorkspace().ClearHistory();
      session->GetWorkspace().ClearArtifacts();
      jv.as_object()["success"] = true;
    } else {
      jv.as_object()["success"] = false;
      jv.as_object()["error"] = "No active session";
    }
  } catch (const std::exception& e) {
    jv.as_object()["success"] = false;
    jv.as_object()["error"] = e.what();
  }
  SendJson(res, 200, jv);
}

void HandleApiWorkspaces(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&&,
                         http::response<http::string_body>& res) {
  boost::json::value resp = boost::json::object{};
  boost::json::array ws_array;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto workspaces = runtime.ListWorkspaces();
    for (const auto& [name, path] : workspaces) {
      boost::json::value item = {
        {"name", name},
        {"path", path}
      };
      ws_array.push_back(item);
    }
  } catch (const std::exception& e) {
    SendJson(res, 500, boost::json::object{{"success", false}, {"error", e.what()}});
    return;
  }
  resp.as_object()["workspaces"] = ws_array;
  SendJson(res, 200, resp);
}

void HandleApiWorkspaceSwitch(Runtime& runtime, std::mutex& io_mutex,
                              http::request<http::string_body>&& req,
                              http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (const std::exception&) {
    boost::json::value err = {{"success", false}, {"error", "Invalid JSON"}};
    SendJson(res, 400, err);
    return;
  }
  if (!json::HasKey(body, "path") || !body.at("path").is_string()) {
    boost::json::value err = {{"success", false}, {"error", "Missing or invalid 'path'"}};
    SendJson(res, 400, err);
    return;
  }
  std::string path_str = boost::json::value_to<std::string>(body.at("path"));
  boost::json::value resp = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    if (runtime.SwitchWorkspace(path_str)) {
      resp.as_object()["success"] = true;
      resp.as_object()["current"] = runtime.GetWorkspaceName();
    } else {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Switch failed";
    }
  } catch (const std::exception& e) {
    resp.as_object()["success"] = false;
    resp.as_object()["error"] = e.what();
  }
  SendJson(res, 200, resp);
}

}  // namespace

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res) {
  auto target = req.target();

  if (target == "/" || target == "/index.html" || target == "/style.css" || target == "/app.js") {
    ServeFile(target, res);
    return;
  }

  if (target == "/api/session" && req.method() == http::verb::get) {
    HandleApiSession(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/history" && req.method() == http::verb::get) {
    HandleApiHistory(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/agents" && req.method() == http::verb::get) {
    HandleApiAgents(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/agent/switch" && req.method() == http::verb::post) {
    HandleApiAgentSwitch(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/clear" && req.method() == http::verb::post) {
    HandleApiClear(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/workspaces" && req.method() == http::verb::get) {
    HandleApiWorkspaces(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/workspace/switch" && req.method() == http::verb::post) {
    HandleApiWorkspaceSwitch(runtime, io_mutex, std::move(req), res);
    return;
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace pu::cli::detail
