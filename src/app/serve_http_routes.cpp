// SPDX-License-Identifier: GPL-3.0-only
#include "serve_internal.hpp"

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

#include "pu/config/agent_summary.hpp"
#include "pu/config/agents.hpp"
#include "pu/config/peer.hpp"
#include "pu/config/serve_registry.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/message.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/projection.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/serve/board.hpp"
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

// The refusal a caller reads names which of the three questions it failed, because they are not
// the same problem: a route this server does not answer from that address, a server that has no
// peer topology to speak of, and a caller holding the wrong secret.
void SendRefusal(http::response<http::string_body>& res, Admission admission,
                 std::string_view target) {
  switch (admission) {
    case Admission::kAllowed:
      return;
    case Admission::kNotFromHere:
      SendError(res, 403, std::string(target) + " is answered on the machine the server runs on");
      return;
    case Admission::kNoTopology:
      SendError(res, 404, "Peer messaging is not configured");
      return;
    case Admission::kWrongToken:
      // `401` rather than `403`: the caller did not say who it was, which is a different thing
      // from saying who it was and being refused. `WWW-Authenticate` names the scheme so a
      // client can retry without reading this source.
      res.set(http::field::www_authenticate, "Bearer");
      SendError(res, 401, std::string(target) + " requires the peer token");
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

  const config::Backend backend = runtime.CurrentBackend();
  boost::json::object body;
  body["agent_name"] = session->Spec().agent_name;
  // Named `gateway`, not `backend_type`: the rest of this file and `agents.json` call the
  // scheme a gateway, and a page that reads one word here and another everywhere else is a
  // page that has to be told twice.
  body["gateway"] = config::GatewayName(backend.gateway);
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
    // Who asked, when it was not the person at the page. Absent rather than empty, so a page
    // reads "the user asked" from the field being there at all.
    if (!msg.from_peer.empty()) item.as_object()["from_peer"] = msg.from_peer;
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
  // is configured. The active agent is the session's, which is what the selector shows.
  Runtime& runtime = call.runtime;
  const std::string active = runtime.DefaultSession() ? runtime.DefaultSession()->Spec().agent_name
                                                      : std::string();
  return config::AgentsAsJson(config::SummarizeAgents(runtime, active));
}

boost::json::value HandleApiAgentSwitch(Call& call, const boost::json::value& body) {
  Runtime& runtime = call.runtime;
  RequireString(body, "agent_name");
  const std::string agent_name = boost::json::value_to<std::string>(body.at("agent_name"));
  const auto* cfg = runtime.Agents().AgentConfig(agent_name);
  if (!cfg) throw NotFound("Agent not found: " + agent_name);
  runtime.SwitchAgent(*cfg);
  return boost::json::object{{"agent", agent_name}};
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
  // A turn a peer asked is not this page's to take back; Conversation says why in its message.
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

// --- The board -----------------------------------------------------------------------------

constexpr std::chrono::seconds kOfflineAfter{45};

serve::Board& BoardOf(const Call& call) {
  // The route table marks the rows that need one, so a null here is a row that forgot its flag
  // rather than a server without a board.
  if (call.board == nullptr) throw NotFound("This server has no board");
  return *call.board;
}

// A board answer is always the job that resulted, plus the outcome when the move settled it.
boost::json::value RenderResult(const serve::Board::ActionResult& result) {
  boost::json::object answer{{"success", true}, {"job", serve::Board::Render(result.job)}};
  if (result.outcome) {
    boost::json::object outcome{{"id", result.outcome->id}, {"state", result.outcome->state}};
    if (result.outcome->result) outcome["result"] = *result.outcome->result;
    answer["outcome"] = std::move(outcome);
  }
  return answer;
}

// A pull is the liveness signal: it carries the token, and the board records the peer it names
// as heard from. Nothing is asked of the caller beyond the secret it already has.
boost::json::value HandleApiJobsGet(Call& call, const boost::json::value&) {
  return BoardOf(call).Snapshot(kOfflineAfter);
}

// What the page draws: the jobs split into the ones still owed and the last few that settled.
boost::json::value HandleApiBoard(Call& call, const boost::json::value&) {
  return BoardOf(call).Page();
}

serve::JobRequest JobFrom(const boost::json::value& body, bool full) {
  serve::JobRequest request;
  request.from = json::OptionalOrDefault<std::string>(body, "from");
  if (!full) {
    request.to = json::OptionalOrDefault<std::string>(body, "to");
    request.intent = json::OptionalOrDefault<std::string>(body, "intent");
    request.due = json::OptionalOrDefault<std::string>(body, "due");
    return request;
  }
  request.job = json::OptionalOrDefault<std::string>(body, "job");
  request.state = json::OptionalOrDefault<std::string>(body, "state");
  request.result = json::OptionalOrDefault<std::string>(body, "result");
  request.question = json::OptionalOrDefault<std::string>(body, "question");
  request.note = json::OptionalOrDefault<std::string>(body, "note");
  return request;
}

boost::json::value HandleApiJobsPost(Call& call, const boost::json::value& body) {
  return RenderResult(BoardOf(call).Post(JobFrom(body, false)));
}

boost::json::value HandleApiJobsPatch(Call& call, const boost::json::value& body) {
  return RenderResult(BoardOf(call).Update(JobFrom(body, true)));
}

// The three verbs a person has over a job a peer holds, in the order the page draws them: answer
// what a blocked job waits for, ask what is happening, and take it back. None is a peer's move,
// so none is `PATCH`: a person has no name to present as `from`.
boost::json::value HandleApiJobsAnswer(Call& call, const boost::json::value& body) {
  const std::string id = json::ValueOrDefault<std::string>(body, "job", "");
  const std::string text = json::ValueOrDefault<std::string>(body, "answer", "");
  if (id.empty()) throw RequestRefused("a board verb needs the job's 'id'");
  if (text.empty()) throw RequestRefused("an answer needs the 'answer' text to send back");
  return RenderResult(BoardOf(call).Answer(id, text));
}

boost::json::value HandleApiJobsChase(Call& call, const boost::json::value& body) {
  const std::string id = json::ValueOrDefault<std::string>(body, "job", "");
  const std::string text = json::ValueOrDefault<std::string>(body, "note", "");
  if (id.empty()) throw RequestRefused("a board verb needs the job's 'id'");
  if (text.empty()) throw RequestRefused("asking about a job needs the 'note' to send");
  return RenderResult(BoardOf(call).Chase(id, text));
}

boost::json::value HandleApiJobsDrop(Call& call, const boost::json::value& body) {
  const std::string id = json::ValueOrDefault<std::string>(body, "job", "");
  const std::string text = json::ValueOrDefault<std::string>(body, "note", "");
  if (id.empty()) throw RequestRefused("a board verb needs the job's 'id'");
  if (text.empty()) throw RequestRefused("taking a job back needs the 'note' that says why");
  return RenderResult(BoardOf(call).Drop(id, text));
}

// An inbound message becomes a turn of its own: it is queued and answered 202, since the
// reply travels back through the sender's send_to_peer tool rather than this response. The
// answer names the turn and who took it, so the sender can watch the board for the outcome.
boost::json::value HandlePeerMessage(Call& call, const boost::json::value& body) {
  if (call.runner == nullptr) throw RuntimeError("This server cannot accept a peer turn");

  const std::string sender = json::ValueOrDefault<std::string>(body, "from", "");
  const std::string message = json::ValueOrDefault<std::string>(body, "message", "");
  // The token says the caller holds the secret;  says which peer it is. A name the topology
  // does not list is refused before a turn is queued, because a turn is what runs tools.
  const auto& peers = call.runtime.Peers();
  if (peers.has_value() && !sender.empty() && !peers->Knows(sender)) {
    throw RequestRefused("Unknown peer: " + sender);
  }
  if (sender.empty()) throw RequestRefused("Missing or invalid 'from'");
  if (message.empty()) throw RequestRefused("Missing or invalid 'message'");

  const std::string job_id = json::ValueOrDefault<std::string>(body, "job", "");
  if (!job_id.empty() && call.board != nullptr) {
    // Claiming before queueing is what makes a job that never runs visible as claimed rather
    // than silently open. A job already taken by someone else is a refusal, not a duplication.
    serve::JobRequest claim;
    claim.job = job_id;
    claim.from = call.runtime.Peers()->self_name;
    claim.state = std::string{serve::JobStateName(serve::JobState::kClaimed)};
    const serve::Board::ActionResult taken = call.board->Update(claim);
    spdlog::info("Peer '{}' job '{}' claimed", sender, job_id);
    if (taken.job.state != serve::JobState::kClaimed) {
      throw RequestRefused("job " + job_id + " did not end up claimed");
    }
  }

  const int hop = json::ValueOrDefault<int>(body, "hop", 0);
  spdlog::info("Peer '{}' sent a message at hop {}: {}", sender, hop, message.substr(0, 120));

  const std::string turn = call.runner->SubmitFromPeer(sender, message, job_id);
  const config::Backend backend = call.runtime.CurrentBackend();
  return boost::json::object{
      {"accepted", true},
      {"from", call.runtime.Peers()->self_name},
      {"turn", turn},
      {"agent", call.runtime.DefaultSession() ? call.runtime.DefaultSession()->Spec().agent_name
                                              : std::string()},
      {"model", backend.model},
  };
}

// Every route this server answers, with the two things a row cannot leave implicit: who may read
// it, and the shape of what it returns. `key` names the member a listing is filed under, so
// `/api/history` answers `{"messages": [...]}` the way `/api/agents` answers `{"agents": [...]}`:
// one envelope, read from this table, instead of a rule each handler restates.
constexpr Route kRoutes[] = {
    //   target,              method,          handler,              key,          audience,                 status, body,  board, board_only
    {"/api/session", http::verb::get, &HandleApiSession, "", Audience::kAnyone, 200, false, false, false},
    {"/api/history", http::verb::get, &HandleApiHistory, "messages", Audience::kAnyone, 200, false, false, false},
    {"/api/agents", http::verb::get, &HandleApiAgents, "agents", Audience::kAnyone, 200, false, false, false},
    {"/api/agent/switch", http::verb::post, &HandleApiAgentSwitch, "", Audience::kAnyone, 200, true, false, false},
    {"/api/rewind", http::verb::post, &HandleApiRewind, "", Audience::kAnyone, 200, true, false, false},
    {"/api/settings", http::verb::get, &HandleApiSettings, "", Audience::kAnyone, 200, false, false, false},
    {"/api/settings", http::verb::post, &HandleApiSetSetting, "", Audience::kAnyone, 200, true, false, false},
    {"/api/settings/reset", http::verb::post, &HandleApiResetSettings, "", Audience::kAnyone, 200, false, false, false},
    {"/api/servers", http::verb::get, &HandleApiServers, "servers", Audience::kLoopback, 200, false, false, false},
    {"/api/board", http::verb::get, &HandleApiBoard, "", Audience::kLoopback, 200, false, true, true},
    {"/api/jobs", http::verb::get, &HandleApiJobsGet, "", Audience::kPeerToken, 200, false, true, true},
    {"/api/jobs", http::verb::post, &HandleApiJobsPost, "", Audience::kPeerToken, 200, true, true, true},
    {"/api/jobs", http::verb::patch, &HandleApiJobsPatch, "", Audience::kPeerToken, 200, true, true, true},
    {"/api/jobs/answer", http::verb::post, &HandleApiJobsAnswer, "", Audience::kPeerToken, 200, true, true, true},
    {"/api/jobs/chase", http::verb::post, &HandleApiJobsChase, "", Audience::kPeerToken, 200, true, true, true},
    {"/api/jobs/drop", http::verb::post, &HandleApiJobsDrop, "", Audience::kPeerToken, 200, true, true, true},
    {"/api/peer/message", http::verb::post, &HandlePeerMessage, "", Audience::kPeerToken, 202, true, false, false},
};

// The bearer a peer presents, or empty when the header is absent or another scheme.
std::string BearerToken(const http::request<http::string_body>& req) {
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return {};
  const std::string_view value = it->value();
  constexpr std::string_view kPrefix = "Bearer ";
  if (value.size() <= kPrefix.size() || value.substr(0, kPrefix.size()) != kPrefix) return {};
  return std::string(value.substr(kPrefix.size()));
}

// The name a token speaks for. A token is one peer's, so a request carrying it is that peer.
std::string PeerForToken(Runtime& runtime, const std::string& token) {
  const auto& peers = runtime.Peers();
  if (!peers.has_value() || token.empty() || !peers->Accepts(token)) return {};
  return peers->self_name;
}

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

Admission Admit(Audience audience, const std::string& address, const std::string& token,
                const std::optional<config::PeerConfig>& peers) {
  switch (audience) {
    case Audience::kAnyone:
      return Admission::kAllowed;
    case Audience::kLoopback:
      // An address a socket could not name is not this machine: refusing it is the only answer
      // that keeps the rule true when the peer is unknown.
      return IsLoopback(address) ? Admission::kAllowed : Admission::kNotFromHere;
    case Audience::kPeerToken:
      if (!peers.has_value()) return Admission::kNoTopology;
      if (token.empty() || !peers->Accepts(token)) return Admission::kWrongToken;
      return Admission::kAllowed;
  }
  return Admission::kAllowed;
}

std::span<const Route> HttpRoutes() { return kRoutes; }

std::string PeerAddress(tcp::socket& socket, beast::error_code& ec) {
  const tcp::endpoint endpoint = socket.remote_endpoint(ec);
  return ec ? std::string{} : endpoint.address().to_string();
}

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex, SessionRunner* runner,
                         serve::Board* board, http::request<http::string_body>&& req,
                         http::response<http::string_body>& res, const std::string& peer,
                         std::uint16_t port) {
  const std::string_view target = req.target();

  if (IsPageFile(target)) {
    ServeFile(std::string(target), res);
    return;
  }

  for (const Route& route : HttpRoutes()) {
    if (target != route.target || req.method() != route.method) continue;

    const std::string token = BearerToken(req);
    const Admission admission = Admit(route.audience, peer, token, runtime.Peers());
    if (admission != Admission::kAllowed) {
      SendRefusal(res, admission, route.target);
      return;
    }
    if (route.needs_board && board == nullptr) {
      SendError(res, 404, "This server has no board");
      return;
    }

    Call call{runtime, board, runner, peer, PeerForToken(runtime, token), IsLoopback(peer), port};

    // A peer's read of the board is how it is heard from, recorded before the snapshot so the
    // answer includes it. It is the one place a read changes anything, and it changes only the
    // record of who is still there — a page's read says nothing about anyone.
    if (route.audience == Audience::kPeerToken && route.needs_board && !call.peer.empty()) {
      board->TouchPeer(call.peer);
    }

    try {
      const boost::json::value body = route.has_body ? ParseBody(req) : boost::json::value{};
      // The board carries its own mutex and shares nothing with the conversation, so a route that
      // only reads or moves it stays off the session's lock. Holding that lock around a peer's
      // poll would let one slow peer stop the page's turn, which is the one thing the split is
      // for. Every other route touches the session and takes it.
      std::optional<std::lock_guard<std::mutex>> session_lock;
      if (!route.board_only) session_lock.emplace(io_mutex);
      SendJson(res, route.status, Wrap(route.key, route.handler(call, body)));
    } catch (const std::exception& e) {
      SendError(res, ErrorStatus(e), e.what());
    }
    return;
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace pu::cli::detail
