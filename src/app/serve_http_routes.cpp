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
#include <string_view>
#include <vector>

#include "pu/agent_manager.hpp"
#include "pu/config/agents.hpp"
#include "pu/context/message.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/tool.hpp"

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

void SendOk(http::response<http::string_body>& res, boost::json::object fields = {}) {
  fields["success"] = true;
  SendJson(res, 200, fields);
}

void SendError(http::response<http::string_body>& res, unsigned status, std::string_view message) {
  SendJson(res, status, boost::json::object{{"success", false}, {"error", std::string(message)}});
}

unsigned ErrorStatus(const std::exception& e) {
  if (dynamic_cast<const Error*>(&e) != nullptr) return 500;
  if (dynamic_cast<const RuntimeError*>(&e) != nullptr) return 400;
  return 500;
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
  if (target == "/") path = base + "/index.html";

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    res.result(http::status::not_found);
    res.prepare_payload();
    return;
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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
  res.set(http::field::cache_control, "no-cache");
  res.prepare_payload();
}

void HandleApiSession(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::object body;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetOrCreateDefaultSession();
    if (!session) {
      SendError(res, 500, "No active session");
      return;
    }
    const config::BackendConfig backend = runtime.CurrentBackend();
    body["agent_name"] = session->GetSpec().agent_name;
    body["backend_type"] = config::BackendTypeName(backend.type);
    body["backend_model"] = backend.model;
    body["backend_host"] = backend.host;
    body["thinking"] = ThinkingLevelName(backend.thinking);
    body["supports_thinking_level"] = runtime.SupportsThinkingLevel();
    if (const auto override_level = runtime.GetThinkingOverride()) {
      body["thinking_override"] = ThinkingLevelName(*override_level);
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendOk(res, std::move(body));
}

void HandleApiHistory(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::array turns;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetOrCreateDefaultSession();
    if (session) {
      Conversation& conversation = session->GetConversation();
      const std::vector<ChatMessage> history = conversation.GetHistory();
      const std::vector<const context::MessageNode*> chain = conversation.GetGraph().Chain();
      for (size_t i = 0; i < history.size(); ++i) {
        const ChatMessage& msg = history[i];
        boost::json::value item = {
            {"id", msg.id},
            {"role", msg.role},
            {"content", msg.content},
            {"timestamp", msg.timestamp},
        };
        if (msg.HasToolCalls()) {
          item.as_object()["tool_calls"] = msg.tool_calls;

          if (i < chain.size()) {
            if (const auto* assistant =
                    std::get_if<context::AssistantPayload>(&chain[i]->payload)) {
              boost::json::array statuses;
              for (const context::ToolCallRecord& record : assistant->tool_calls) {
                statuses.push_back(
                    record.status == context::ToolCallStatus::kCompleted ? "done" : "pending");
              }
              item.as_object()["tool_call_status"] = std::move(statuses);
            }
          }
        }
        if (!msg.tool_call_id.empty()) item.as_object()["tool_call_id"] = msg.tool_call_id;
        if (!msg.tool_name.empty()) item.as_object()["tool_name"] = msg.tool_name;
        if (!msg.reasoning_content.empty())
          item.as_object()["reasoning_content"] = msg.reasoning_content;

        if (msg.role == context::kToolRole) {
          const tools::ToolResult parsed = tools::ParseToolResult(msg.content);
          item.as_object()["output"] = parsed.valid ? parsed.stdout_content : msg.content;
          item.as_object()["error"] = parsed.error;
        }

        turns.push_back(std::move(item));
      }
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendJson(res, 200, turns);
}

void HandleApiAgents(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&&,
                     http::response<http::string_body>& res) {
  boost::json::array agents;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    for (const auto& name : runtime.GetAgentManager().GetAgentNames()) {
      const auto* cfg = runtime.GetAgentManager().GetAgentConfig(name);
      agents.push_back(boost::json::value{{"name", name},
                                          {"description", cfg ? cfg->description : std::string{}}});
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendJson(res, 200, boost::json::object{{"agents", std::move(agents)}});
}

void HandleApiAgentSwitch(Runtime& runtime, std::mutex& io_mutex,
                          http::request<http::string_body>&& req,
                          http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (const std::exception&) {
    SendError(res, 400, "Invalid JSON");
    return;
  }
  if (!json::HasKey(body, "agent_name") || !body.at("agent_name").is_string()) {
    SendError(res, 400, "Missing or invalid 'agent_name'");
    return;
  }
  const std::string agent_name = boost::json::value_to<std::string>(body.at("agent_name"));
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto* cfg = runtime.GetAgentManager().GetAgentConfig(agent_name);
    if (!cfg) {
      SendError(res, 404, "Agent not found: " + agent_name);
      return;
    }
    runtime.SwitchAgent(*cfg);
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendOk(res, boost::json::object{{"agent", agent_name}});
}

void HandleApiClear(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&&,
                    http::response<http::string_body>& res) {
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    runtime.ClearConversation();
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendOk(res);
}

void HandleApiWorkspaces(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&&,
                         http::response<http::string_body>& res) {
  boost::json::array workspaces;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    for (const auto& [name, path] : runtime.ListWorkspaces()) {
      workspaces.push_back(boost::json::value{{"name", name}, {"path", path}});
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendJson(res, 200, boost::json::object{{"workspaces", std::move(workspaces)}});
}

void HandleApiRewind(Runtime& runtime, std::mutex& io_mutex, http::request<http::string_body>&& req,
                     http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (const std::exception&) {
    SendError(res, 400, "Invalid JSON");
    return;
  }
  const int turn = json::ValueOrDefault<int>(body, "turn", 0);
  if (turn < 1) {
    SendError(res, 400, "Missing or invalid 'turn'");
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    if (!runtime.RewindBefore(static_cast<size_t>(turn))) {
      SendError(res, 400, "No such turn");
      return;
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendOk(res);
}

void HandleApiThinking(Runtime& runtime, std::mutex& io_mutex,
                       http::request<http::string_body>&& req,
                       http::response<http::string_body>& res) {
  boost::json::value requested;
  try {
    requested = boost::json::parse(req.body());
  } catch (const std::exception&) {
    SendError(res, 400, "Invalid JSON");
    return;
  }
  boost::json::object body;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    if (!json::HasKey(requested, "level") || !requested.at("level").is_string()) {
      SendError(res, 400, "Missing or invalid 'level'");
      return;
    }
    const std::string level = boost::json::value_to<std::string>(requested.at("level"));

    if (level != "auto" && level != "default" &&
        ParseThinkingLevel(level) == ThinkingLevel::kServerDefault) {
      SendError(res, 400, "Unknown thinking level");
      return;
    }
    const bool accepted = runtime.SetThinkingLevel(
        level == "auto" ? std::nullopt : std::optional<ThinkingLevel>(ParseThinkingLevel(level)));
    if (!accepted) {
      SendError(res, 400, "This backend does not carry a thinking level");
      return;
    }
    body["thinking"] = ThinkingLevelName(runtime.CurrentThinkingLevel());
    if (const auto override_level = runtime.GetThinkingOverride()) {
      body["thinking_override"] = ThinkingLevelName(*override_level);
    }
  } catch (const std::exception& e) {
    SendError(res, ErrorStatus(e), e.what());
    return;
  }
  SendOk(res, std::move(body));
}

using Handler = void (*)(Runtime&, std::mutex&, http::request<http::string_body>&&,
                         http::response<http::string_body>&);

struct Route {
  std::string_view target;
  http::verb method;
  Handler handler;
};

constexpr Route kRoutes[] = {
    {"/api/session", http::verb::get, &HandleApiSession},
    {"/api/history", http::verb::get, &HandleApiHistory},
    {"/api/agents", http::verb::get, &HandleApiAgents},
    {"/api/agent/switch", http::verb::post, &HandleApiAgentSwitch},
    {"/api/clear", http::verb::post, &HandleApiClear},
    {"/api/rewind", http::verb::post, &HandleApiRewind},
    {"/api/thinking", http::verb::post, &HandleApiThinking},
    {"/api/workspaces", http::verb::get, &HandleApiWorkspaces},
};

}  // namespace

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res) {
  const std::string_view target = req.target();

  if (target == "/" || target == "/index.html" || target == "/style.css" || target == "/app.js") {
    ServeFile(std::string(target), res);
    return;
  }

  for (const Route& route : kRoutes) {
    if (target == route.target && req.method() == route.method) {
      route.handler(runtime, io_mutex, std::move(req), res);
      return;
    }
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace pu::cli::detail
