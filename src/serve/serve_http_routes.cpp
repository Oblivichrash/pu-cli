// SPDX-License-Identifier: GPL-3.0-only
#include "serve/serve_internal.hpp"

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
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

#include "pu/app/agent_summary.hpp"
#include "pu/config/agents.hpp"
#include "pu/config/serve_registry.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/message.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/llm/projection.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/tool.hpp"

namespace pu::serve::detail {
namespace {

// One value out of a query string, read without decoding: a gateway name is a plain word, and
// the only parameter these handlers ask for is one. Absent or empty answers empty.
std::string QueryValue(const std::string& query, std::string_view name) {
  std::size_t at = 0;
  while (at < query.size()) {
    const std::size_t end = query.find('&', at);
    const std::string_view pair(query.data() + at,
                               (end == std::string::npos ? query.size() : end) - at);
    const std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
      return std::string(pair.substr(eq + 1));
    }
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return {};
}

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

// The refusal a caller reads names which of the two questions it failed, because they are not
// the same problem: a route this server does not answer from that address, or a route it does
// not answer at all.
void SendRefusal(http::response<http::string_body>& res, Admission admission,
                 std::string_view target) {
  switch (admission) {
    case Admission::kAllowed:
      return;
    case Admission::kNotFromHere:
      SendError(res, 403, std::string(target) + " is answered on the machine the server runs on");
      return;
  }
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

// A page file is whatever the page directory holds, so a new web/*.js is served without a
// second list to remember. The name is checked, not the existence: a name that is not in
// the directory reaches ServeFile and takes its 404, and a name that is a path never does,
// because a plain name has no parent.
bool IsPageFile(std::string_view target) {
  if (target == "/") return true;
  const std::filesystem::path relative{std::string(target.substr(1))};
  if (relative.empty() || relative.has_parent_path() || relative.has_root_path()) return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(GetWebDir()) / relative, ec);
}

}  // namespace

// Declared ahead of IsPageFile, which asks what the page directory holds before ServeFile
// resolves it again for the read.
const std::string& GetWebDir();

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
  res.set(http::field::cache_control, "no-cache");
  const std::string extension = path.extension().string();
  if (extension == ".html")
    res.set(http::field::content_type, "text/html");
  else if (extension == ".css")
    res.set(http::field::content_type, "text/css");
  else if (extension == ".js")
    res.set(http::field::content_type, "application/javascript");
  else
    res.set(http::field::content_type, "application/octet-stream");
  res.prepare_payload();
}

// --- Session and conversation --------------------------------------------------------------

boost::json::value HandleApiSession(Call& call, const boost::json::value&) {
  Runtime& runtime = call.runtime;
  auto session = runtime.DefaultSession();
  if (!session) throw GenericError("No active session");

  const model::Backend backend = runtime.CurrentBackend();
  boost::json::object body;
  body["agent_name"] = session->Spec().agent_name;
  // Named `gateway`, not `backend_type`: the rest of this file and `agents.json` call the
  // scheme a gateway, and a page that reads one word here and another everywhere else is a
  // page that has to be told twice.
  body["gateway"] = model::GatewayName(backend.gateway);
  body["backend_model"] = backend.model;
  body["backend_host"] = backend.host;
  // How much context the model holds, `0` when nobody stated one. The page draws its bar
  // against this and shows the count alone when it is absent, so a guess never becomes a fill.
  body["context_window"] = runtime.ContextWindow();
  body["thinking"] = ThinkingLevelName(backend.thinking);
  if (const auto override_level = runtime.ThinkingOverride()) {
    body["thinking_override"] = ThinkingLevelName(*override_level);
  }
  // The server answers with the name it goes by, because the page cannot derive it: the directory
  // it serves is a fact of this machine, and a path is not something a remote reader is handed.
  // The page that reached this port is the server that answers here, so `port` is what the page
  // already dialled and `name` is the label to draw over it.
  body["server"] = boost::json::object{
      {"name", config::WorkspaceLabel(runtime.WorkspaceRoot().string())},
      {"port", call.port},
  };
  // Every setting this build knows, with the value in force and where it came from; the two
  // fields above are the same answer in the shape older pages read.
  boost::json::array settings;
  for (const session::SettingView& view : runtime.Settings()) {
    settings.push_back(boost::json::object{
        {"name", view.name},
        {"type", view.type},
        {"value", view.value},
        {"source", view.source},
        {"agent_value", view.agent_value},
    });
  }
  body["settings"] = std::move(settings);

  // What this conversation has spent so far. The counts are sums; a price is a sum only while
  // every turn that reported one reported it in the same unit, which is why `cost` can be
  // absent even when turns carried prices.
  body["usage_total"] = session::UsageLedger::ToJson(session->Usage().Total());
  boost::json::array tools;
  for (const std::string& name : runtime.Tools().ToolNames()) {
    tools.emplace_back(name);
  }
  body["tools"] = std::move(tools);
  return body;
}

boost::json::value HandleApiHistory(Call& call, const boost::json::value&) {
  boost::json::array turns;
  auto session = call.runtime.DefaultSession();
  if (!session) return turns;

  Conversation& conversation = session->Messages();
  const session::UsageLedger& conversation_usage = session->Usage();
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

    if (msg.role == kToolRole) {
      const tools::ToolResult parsed = tools::ParseToolResult(msg.content);
      item.as_object()["output"] = parsed.valid ? parsed.output : msg.content;
      item.as_object()["error"] = parsed.error;
      // The same field the live frame carries, so a reloaded turn draws a page as a page.
      if (parsed.valid && !parsed.page.is_null()) item.as_object()["page"] = parsed.page;
    }

    // What this turn cost, beside it rather than inside it. Absent for a turn that recorded
    // nothing — a session from before usage was kept, or one whose provider reported none.
    if (const std::optional<session::TurnUsage> usage = conversation_usage.For(node.id)) {
      item.as_object()["usage"] = session::UsageLedger::ToJson(*usage);
    }

    turns.push_back(std::move(item));
  }
  return turns;
}

boost::json::value HandleApiAgents(Call& call, const boost::json::value&) {
  // The same summary `pu agents` prints, so the page and the CLI cannot disagree about what
  // is configured. One agent file, so the answer is one object rather than a list of one.
  Runtime& runtime = call.runtime;
  return app::AgentAsJson(app::SummarizeAgent(runtime.Agents()));
}

// The backend the agent speaks through now. What else it could speak through is not here: the
// gateways are this build's own table and the models belong to one gateway, so each is asked at
// its own address -- `/api/gateways`, `/api/models` -- by the menu that needs it, rather than
// carried along on every read of the backend in force.
boost::json::value BackendAsJson(Call& call) {
  const model::Backend current = call.runtime.CurrentBackend();
  return boost::json::object{
      {"gateway", model::GatewayName(current.gateway)},
      {"model", current.model},
      {"host", current.host},
  };
}

boost::json::value HandleApiBackend(Call& call, const boost::json::value&) {
  return BackendAsJson(call);
}

// Every gateway this build can speak through, in table order. The list is the code's own, so
// this asks a provider nothing.
boost::json::value HandleApiGateways(Call&, const boost::json::value&) {
  boost::json::array gateways;
  for (const model::Gateway& gateway : model::Gateways()) {
    gateways.emplace_back(std::string(gateway.name));
  }
  return boost::json::object{{"gateways", std::move(gateways)}};
}

// What one gateway serves, asked when the page opens the model list rather than once at load.
// The gateway is named by the request, so the answer is that gateway's catalogue and not the
// current backend's; the key is the one a turn would read from the environment, which keeps a
// key out of the page. A gateway that cannot be asked answers with no models and the reason,
// and the page falls back to typing a model name.
boost::json::value HandleApiModels(Call& call, const boost::json::value&) {
  const std::string gateway_name = QueryValue(call.query, "gateway");
  boost::json::array models;
  std::string error;
  const model::Gateway* gateway = model::FindGateway(gateway_name);
  if (gateway == nullptr) {
    error = "No gateway is named '" + gateway_name + "'.";
  } else {
    model::Backend backend;
    backend.gateway = gateway->id;
    backend.host = std::string(gateway->default_host);
    const std::string key_env(gateway->api_key_env);
    if (!key_env.empty()) {
      if (const char* key = std::getenv(key_env.c_str()); key != nullptr && *key != '\0') {
        backend.api_key = std::string(key);
      }
    }
    try {
      for (const model::ModelEntry& entry : llm::ListModels(backend, call.runtime.Http())) {
        models.emplace_back(entry.id);
      }
    } catch (const std::exception& e) {
      error = e.what();
    }
  }
  return boost::json::object{
      {"gateway", gateway_name},
      {"models", std::move(models)},
      {"error", error},
  };
}

boost::json::value HandleApiSetBackend(Call& call, const boost::json::value& body) {
  const std::string gateway = json::ValueOrDefault<std::string>(body, "gateway", "");
  const std::string model = json::ValueOrDefault<std::string>(body, "model", "");
  if (gateway.empty()) throw RequestRefused("Missing 'gateway'");
  if (model.empty()) throw RequestRefused("Missing 'model'");
  const std::optional<model::GatewayId> id = model::ParseGatewayId(gateway);
  if (!id) throw RequestRefused("Unknown gateway: " + gateway);
  model::Backend next = call.runtime.CurrentBackend();
  if (next.gateway != *id) next.host = model::DefaultHostFor(*id);
  next.gateway = *id;
  next.model = model;
  call.runtime.SwitchBackend(next);
  return BackendAsJson(call);
}

// The other servers on this machine, so the page can name a workspace instead of a port. The one
// that answered is not in the list: it is the page's own, and offering a switch to where the
// reader already is is a control that does nothing. The entry carries a label and a port, never a
// directory, so what crosses the wire is the name a project goes by rather than where it lives.
boost::json::value HandleApiServers(Call&, const boost::json::value&) {
  boost::json::array servers;
  for (const config::ServeRecord& record :
       config::OthersAmong(config::LiveServeRecords(), platform::CurrentProcessId())) {
    servers.push_back(boost::json::object{
        {"name", config::WorkspaceLabel(record.directory)},
        {"port", record.port},
    });
  }
  return servers;
}

boost::json::value HandleApiRewind(Call& call, const boost::json::value& body) {
  const int turn = json::ValueOrDefault<int>(body, "turn", 0);
  if (turn < 1) throw RequestRefused("Missing or invalid 'turn'");
  if (!call.runtime.RewindBefore(static_cast<size_t>(turn))) throw RequestRefused("No such turn");
  return boost::json::object{};
}

// The settings this build knows, with the value in force and where it came from. One shape for
// the whole table, so a page draws a new setting without a new endpoint.
boost::json::value SettingsBody(Runtime& runtime) {
  boost::json::array settings;
  for (const session::SettingView& view : runtime.Settings()) {
    settings.push_back(boost::json::object{
        {"name", view.name},
        {"type", view.type},
        {"value", view.value},
        {"source", view.source},
        {"agent_value", view.agent_value},
    });
  }
  return boost::json::object{{"settings", std::move(settings)}};
}

boost::json::value HandleApiSettings(Call& call, const boost::json::value&) {
  return SettingsBody(call.runtime);
}

boost::json::value HandleApiSetSetting(Call& call, const boost::json::value& body) {
  RequireString(body, "name");
  const std::string name = boost::json::value_to<std::string>(body.at("name"));
  // Absent `value` and an explicit null mean the same thing: clear this override. Only the
  // member being there with something else in it is a value to set.
  const boost::json::value raw =
      json::HasKey(body, "value") ? body.at("value") : boost::json::value();
  call.runtime.SetSetting(name, raw);
  return SettingsBody(call.runtime);
}

boost::json::value HandleApiResetSettings(Call& call, const boost::json::value&) {
  call.runtime.ClearSettings();
  return SettingsBody(call.runtime);
}

// Every route this server answers, with the two things a row cannot leave implicit: who may read
// it, and the shape of what it returns. `key` names the member a listing is filed under, so
// `/api/history` answers `{"messages": [...]}` the way `/api/backend` answers `{"backend": {...}}`:
// one envelope, read from this table, instead of a rule each handler restates.
constexpr Route kRoutes[] = {
    //   target,              method,          handler,              key,          audience,                 status, body
    {"/api/session", http::verb::get, &HandleApiSession, "", Audience::kAnyone, 200, false},
    {"/api/history", http::verb::get, &HandleApiHistory, "messages", Audience::kAnyone, 200, false},
    // The page reads one agent's backend, and writes it back when a choice is made there. Both
    // go through /api/backend: nothing on the page enumerates agents, because there is one.
    {"/api/backend", http::verb::get, &HandleApiBackend, "backend", Audience::kAnyone, 200, false},
    {"/api/backend", http::verb::post, &HandleApiSetBackend, "backend", Audience::kAnyone, 200, false},
    {"/api/backend", http::verb::get, &HandleApiBackend, "", Audience::kAnyone, 200, false},
    {"/api/backend", http::verb::post, &HandleApiSetBackend, "", Audience::kAnyone, 200, true},
    {"/api/gateways", http::verb::get, &HandleApiGateways, "", Audience::kAnyone, 200, false},
    {"/api/models", http::verb::get, &HandleApiModels, "", Audience::kAnyone, 200, false},
    {"/api/rewind", http::verb::post, &HandleApiRewind, "", Audience::kAnyone, 200, true},
    {"/api/settings", http::verb::get, &HandleApiSettings, "", Audience::kAnyone, 200, false},
    {"/api/settings", http::verb::post, &HandleApiSetSetting, "", Audience::kAnyone, 200, true},
    {"/api/settings/reset", http::verb::post, &HandleApiResetSettings, "", Audience::kAnyone, 200, false},
    {"/api/servers", http::verb::get, &HandleApiServers, "servers", Audience::kLoopback, 200, false},
};

// The one envelope every route answers in: the handler's payload under the key its row names, or
// the handler's own object when the row names none.
boost::json::value Wrap(std::string_view key, boost::json::value result) {
  if (!key.empty()) {
    return boost::json::object{{std::string(key), std::move(result)}, {"success", true}};
  }
  boost::json::object body = result.is_object() ? std::move(result.as_object())
                                                : boost::json::object{};
  body["success"] = true;
  return body;
}

}  // namespace

bool IsLoopback(const std::string& address) {
  return address.rfind("127.", 0) == 0 || address == "::1" || address == "::ffff:127.0.0.1";
}

Admission Admit(Audience audience, const std::string& address) {
  switch (audience) {
    case Audience::kAnyone:
      return Admission::kAllowed;
    case Audience::kLoopback:
      // An address a socket could not name is not this machine: refusing it is the only answer
      // that keeps the rule true when the caller is unknown.
      return IsLoopback(address) ? Admission::kAllowed : Admission::kNotFromHere;
  }
  return Admission::kAllowed;
}

std::span<const Route> HttpRoutes() { return kRoutes; }

std::string PeerAddress(tcp::socket& socket, beast::error_code& ec) {
  const tcp::endpoint endpoint = socket.remote_endpoint(ec);
  return ec ? std::string{} : endpoint.address().to_string();
}

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res, const std::string& peer,
                         std::uint16_t port) {
  const std::string_view request_target = req.target();
  const std::size_t question = request_target.find('?');
  const std::string_view target = request_target.substr(0, question);
  const std::string_view query =
      question == std::string_view::npos ? std::string_view{} : request_target.substr(question + 1);

  if (IsPageFile(target)) {
    ServeFile(std::string(target), res);
    return;
  }

  for (const Route& route : HttpRoutes()) {
    if (target != route.target || req.method() != route.method) continue;

    const Admission admission = Admit(route.audience, peer);
    if (admission != Admission::kAllowed) {
      SendRefusal(res, admission, route.target);
      return;
    }

    Call call{.query = std::string(query), .runtime = runtime, .address = peer, .loopback = IsLoopback(peer), .port = port};

    try {
      const boost::json::value body = route.has_body ? ParseBody(req) : boost::json::value{};
      std::lock_guard<std::mutex> session_lock(io_mutex);
      SendJson(res, route.status, Wrap(route.key, route.handler(call, body)));
    } catch (const std::exception& e) {
      SendError(res, ErrorStatus(e), e.what());
    }
    return;
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace pu::serve::detail
