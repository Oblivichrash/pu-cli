// SPDX-License-Identifier: GPL-3.0-only
#include "serve_internal.hpp"

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <condition_variable>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "pu/config/agents.hpp"
#include "pu/session/message.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/projection.hpp"
#include "pu/runtime/runtime.hpp"
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

void SendError(http::response<http::string_body>& res, unsigned status, std::string_view message) {
  SendJson(res, status, boost::json::object{{"success", false}, {"error", std::string(message)}});
}

unsigned ErrorStatus(const std::exception& e) {
  if (dynamic_cast<const NotFound*>(&e) != nullptr) return 404;
  if (dynamic_cast<const GenericError*>(&e) != nullptr) return 500;
  if (dynamic_cast<const RuntimeError*>(&e) != nullptr) return 400;
  return 500;
}

const char* ViewStatus(session::ToolCallStatus status) {
  switch (status) {
    case session::ToolCallStatus::kCompleted:
      return "done";
    case session::ToolCallStatus::kInterrupted:
      return "interrupted";
    case session::ToolCallStatus::kPending:
      return "pending";
  }
  return "pending";
}

boost::json::value ParseBody(const http::request<http::string_body>& req) {
  if (req.body().empty()) return boost::json::object{};
  try {
    return boost::json::parse(req.body());
  } catch (const boost::system::system_error&) {
    throw RequestRefused("Invalid JSON");
  }
}

void RequireString(const boost::json::value& body, std::string_view key) {
  if (!json::HasKey(body, key) || !body.at(key).is_string()) {
    throw RequestRefused("Missing or invalid '" + std::string(key) + "'");
  }
}

constexpr std::string_view kPageFiles[] = {
    "/",            "/index.html",    "/style.css",       "/app.js",
    "/state.js",    "/approval.js",   "/layout.js",       "/block-thinking.js",
    "/block-tool.js", "/block-node.js", "/message.js",    "/transcript.js",
    "/panel.js",    "/notice.js",     "/history.js",      "/ws.js",
    "/session.js",  "/composer.js",
};

bool IsPageFile(std::string_view target) {
  for (const std::string_view page : kPageFiles) {
    if (target == page) return true;
  }
  return false;
}

}  // namespace

std::filesystem::path ExecutableDir() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
  std::error_code ec;
  auto resolved = std::filesystem::weakly_canonical(buffer.data(), ec);
  return ec ? std::filesystem::path(buffer.data()).parent_path() : resolved.parent_path();
#else
  std::error_code ec;
  auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : resolved.parent_path();
#endif
}

const std::string& GetWebDir() {
  // Re-read per call rather than cached under call_once: the answer names a directory
  // the caller may have just pointed PU_WEB_DIR at, and a value frozen at the first
  // request outlives the environment it was read from.
  static std::string dir;
  static std::mutex dir_mutex;
  std::lock_guard<std::mutex> lock(dir_mutex);
  {
    const char* env = std::getenv("PU_WEB_DIR");
    if (env && *env) {
      dir = env;
      return dir;
    }
    std::vector<std::filesystem::path> candidates;
    if (const std::filesystem::path exe_dir = ExecutableDir(); !exe_dir.empty()) {
      candidates.push_back(exe_dir / "web");
      candidates.push_back(exe_dir / ".." / "web");
      candidates.push_back(exe_dir / ".." / "share" / "pu" / "web");
    }
    candidates.push_back("./web");
    candidates.push_back("../share/pu/web");
    candidates.push_back("/usr/share/pu/web");
    candidates.push_back("/usr/local/share/pu/web");
    for (const auto& d : candidates) {
      if (std::filesystem::exists(d) && std::filesystem::is_directory(d)) {
        dir = d.string();
        return dir;
      }
    }
    dir = "./web";
  }
  return dir;
}

namespace {

void ServeFile(const std::string& target, http::response<http::string_body>& res) {
  const std::filesystem::path base = GetWebDir();
  const std::filesystem::path path = base / (target == "/" ? "index.html" : target.substr(1));

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    res.result(http::status::not_found);
    res.prepare_payload();
    return;
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  res.result(http::status::ok);
  res.body() = std::move(content);
  const std::string extension = path.extension().string();
  if (extension == ".html")
    res.set(http::field::content_type, "text/html");
  else if (extension == ".css")
    res.set(http::field::content_type, "text/css");
  else if (extension == ".js")
    res.set(http::field::content_type, "application/javascript");
  else
    res.set(http::field::content_type, "application/octet-stream");
  res.set(http::field::cache_control, "no-cache");
  res.prepare_payload();
}

boost::json::value HandleApiSession(Runtime& runtime, const boost::json::value&) {
  auto session = runtime.DefaultSession();
  if (!session) throw GenericError("No active session");

  const config::Backend backend = runtime.CurrentBackend();
  boost::json::object body;
  body["agent_name"] = session->Spec().agent_name;
  body["backend_type"] = config::GatewayName(backend.gateway);
  body["backend_model"] = backend.model;
  body["backend_host"] = backend.host;
  body["thinking"] = ThinkingLevelName(backend.thinking);
  if (const auto override_level = runtime.ThinkingOverride()) {
    body["thinking_override"] = ThinkingLevelName(*override_level);
  }
  boost::json::array tools;
  for (const std::string& name : runtime.Tools().ToolNames()) {
    tools.emplace_back(name);
  }
  body["tools"] = std::move(tools);
  return body;
}

boost::json::value HandleApiHistory(Runtime& runtime, const boost::json::value&) {
  boost::json::array turns;
  auto session = runtime.DefaultSession();
  if (!session) return turns;

  Conversation& conversation = session->Messages();
  const std::vector<const session::MessageNode*> chain = conversation.Chain();
  for (size_t i = 0; i < chain.size(); ++i) {
    const session::MessageNode& node = *chain[i];
    const ChatMessage msg = session::RenderMessage(node, static_cast<int>(i) + 1);
    boost::json::value item = {
        {"id", msg.id},
        {"role", msg.role},
        {"content", msg.content},
        {"timestamp", msg.timestamp},
    };
    if (msg.HasToolCalls()) {
      item.as_object()["tool_calls"] = msg.tool_calls;

      if (const auto* assistant = std::get_if<session::AssistantPayload>(&node.payload)) {
        boost::json::array statuses;
        for (const session::ToolCallRecord& record : assistant->tool_calls) {
          statuses.push_back(ViewStatus(record.status));
        }
        item.as_object()["tool_call_status"] = std::move(statuses);
      }
    }
    if (!msg.tool_call_id.empty()) item.as_object()["tool_call_id"] = msg.tool_call_id;
    if (!msg.tool_name.empty()) item.as_object()["tool_name"] = msg.tool_name;
    if (!msg.reasoning.empty()) {
      item.as_object()["reasoning"] = llm::ProjectReasoning(msg.reasoning);
    }

    if (msg.role == session::kToolRole) {
      const tools::ToolResult parsed = tools::ParseToolResult(msg.content);
      item.as_object()["output"] = parsed.valid ? parsed.output : msg.content;
      item.as_object()["error"] = parsed.error;
    }

    turns.push_back(std::move(item));
  }
  return turns;
}

boost::json::value HandleApiAgents(Runtime& runtime, const boost::json::value&) {
  boost::json::array agents;
  for (const auto& name : runtime.Agents().AgentNames()) {
    const auto* cfg = runtime.Agents().AgentConfig(name);
    agents.push_back(
        boost::json::value{{"name", name}, {"description", cfg ? cfg->description : std::string{}}});
  }
  return agents;
}

boost::json::value HandleApiAgentSwitch(Runtime& runtime, const boost::json::value& body) {
  RequireString(body, "agent_name");
  const std::string agent_name = boost::json::value_to<std::string>(body.at("agent_name"));
  const auto* cfg = runtime.Agents().AgentConfig(agent_name);
  if (!cfg) throw NotFound("Agent not found: " + agent_name);
  runtime.SwitchAgent(*cfg);
  return boost::json::object{{"agent", agent_name}};
}

boost::json::value HandleApiClear(Runtime& runtime, const boost::json::value&) {
  runtime.ClearConversation();
  return boost::json::object{};
}

boost::json::value HandleApiWorkspaces(Runtime& runtime, const boost::json::value&) {
  boost::json::array workspaces;
  for (const auto& [name, path] : runtime.ListWorkspaces()) {
    workspaces.push_back(boost::json::value{{"name", name}, {"path", path}});
  }
  return workspaces;
}

boost::json::value HandleApiRewind(Runtime& runtime, const boost::json::value& body) {
  const int turn = json::ValueOrDefault<int>(body, "turn", 0);
  if (turn < 1) throw RequestRefused("Missing or invalid 'turn'");
  if (!runtime.RewindBefore(static_cast<size_t>(turn))) throw RequestRefused("No such turn");
  return boost::json::object{};
}

boost::json::value HandleApiThinking(Runtime& runtime, const boost::json::value& requested) {
  RequireString(requested, "level");
  const std::string level = boost::json::value_to<std::string>(requested.at("level"));
  if (level != "auto" && level != "default" &&
      ParseThinkingLevel(level) == ThinkingLevel::kServerDefault) {
    throw RequestRefused("Unknown thinking level");
  }

  runtime.SetThinkingLevel(
      level == "auto" ? std::nullopt : std::optional<ThinkingLevel>(ParseThinkingLevel(level)));

  boost::json::object body;
  body["thinking"] = ThinkingLevelName(runtime.CurrentThinkingLevel());
  if (const auto override_level = runtime.ThinkingOverride()) {
    body["thinking_override"] = ThinkingLevelName(*override_level);
  }
  return body;
}

using Handler = boost::json::value (*)(Runtime&, const boost::json::value& body);

enum class Envelope { kObject, kArray, kAgents, kWorkspaces };

struct Route {
  std::string_view target;
  http::verb method;
  Handler handler;
  Envelope envelope;
  bool has_body;
};

constexpr Route kRoutes[] = {
    {"/api/session", http::verb::get, &HandleApiSession, Envelope::kObject, false},
    {"/api/history", http::verb::get, &HandleApiHistory, Envelope::kArray, false},
    {"/api/agents", http::verb::get, &HandleApiAgents, Envelope::kAgents, false},
    {"/api/agent/switch", http::verb::post, &HandleApiAgentSwitch, Envelope::kObject, true},
    {"/api/clear", http::verb::post, &HandleApiClear, Envelope::kObject, false},
    {"/api/rewind", http::verb::post, &HandleApiRewind, Envelope::kObject, true},
    {"/api/thinking", http::verb::post, &HandleApiThinking, Envelope::kObject, true},
    {"/api/workspaces", http::verb::get, &HandleApiWorkspaces, Envelope::kWorkspaces, false},
};

boost::json::value Wrap(Envelope envelope, boost::json::value result) {
  switch (envelope) {
    case Envelope::kArray:
      return result;
    case Envelope::kAgents:
      return boost::json::object{{"agents", std::move(result)}};
    case Envelope::kWorkspaces:
      return boost::json::object{{"workspaces", std::move(result)}};
    case Envelope::kObject:
      break;
  }
  boost::json::object body = result.is_object() ? std::move(result.as_object())
                                                : boost::json::object{};
  body["success"] = true;
  return body;
}

}  // namespace

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res) {
  const std::string_view target = req.target();

  if (IsPageFile(target)) {
    ServeFile(std::string(target), res);
    return;
  }

  for (const Route& route : kRoutes) {
    if (target != route.target || req.method() != route.method) continue;

    try {
      const boost::json::value body = route.has_body ? ParseBody(req) : boost::json::value{};
      std::lock_guard<std::mutex> lock(io_mutex);
      SendJson(res, 200, Wrap(route.envelope, route.handler(runtime, body)));
    } catch (const std::exception& e) {
      SendError(res, ErrorStatus(e), e.what());
    }
    return;
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace pu::cli::detail
