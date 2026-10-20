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

#include "pu/config/agent_summary.hpp"
#include "pu/config/agents.hpp"
#include "pu/config/peer.hpp"
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
    "/panel.js",    "/board.js",      "/notice.js",       "/history.js",
    "/ws.js",       "/session.js",    "/composer.js",
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
  // The page's own files are small and local: revalidated on every load rather than
  // cached, so a rebuilt binary never runs against a browser's stale copy of the
  // script that drives it. The CDN libraries keep their own caching.
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
  const session::SessionUsageTotal total = session->Usage().Total();
  boost::json::object usage_total{
      {"turns", total.turns},
      {"rounds", total.rounds},
      {"prompt_tokens", total.prompt_tokens},
      {"completion_tokens", total.completion_tokens},
      {"cached_tokens", total.cached_tokens},
      {"priced_turns", total.priced_turns},
  };
  if (total.cost) {
    usage_total["cost"] = boost::json::object{
        {"amount", total.cost->amount},
        {"unit", total.cost->unit},
    };
  }
  body["usage_total"] = std::move(usage_total);
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
      item.as_object()["usage"] = boost::json::object{
          {"prompt_tokens", usage->prompt_tokens},
          {"completion_tokens", usage->completion_tokens},
          {"cached_tokens", usage->cached_tokens},
          {"rounds", usage->rounds},
          {"model", usage->model},
          {"gateway", usage->gateway},
      };
      // A price only where the product reported one. Absent and zero are different answers:
      // a call a product priced at nothing is not a product that prices nothing.
      if (usage->cost) {
        item.as_object()["usage"].as_object()["cost"] = boost::json::object{
            {"amount", usage->cost->amount},
            {"unit", usage->cost->unit},
        };
      }
    }

    turns.push_back(std::move(item));
  }
  return turns;
}

boost::json::value HandleApiAgents(Runtime& runtime, const boost::json::value&) {
  // The same summary `pu agents` prints, so the page and the CLI cannot disagree about what
  // is configured. The active agent is the session's, which is what the selector shows.
  const std::string active = runtime.DefaultSession() ? runtime.DefaultSession()->Spec().agent_name
                                                      : std::string();
  return config::AgentsAsJson(config::SummarizeAgents(runtime, active));
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
  // A turn a peer asked is not this page's to take back; Conversation says why in its message.
  if (!runtime.RewindBefore(static_cast<size_t>(turn))) throw RequestRefused("No such turn");
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

boost::json::value HandleApiSettings(Runtime& runtime, const boost::json::value&) {
  return SettingsBody(runtime);
}

boost::json::value HandleApiSetSetting(Runtime& runtime, const boost::json::value& body) {
  RequireString(body, "name");
  const std::string name = boost::json::value_to<std::string>(body.at("name"));
  // Absent `value` and an explicit null mean the same thing: clear this override. Only the
  // member being there with something else in it is a value to set.
  const boost::json::value raw =
      json::HasKey(body, "value") ? body.at("value") : boost::json::value();
  runtime.SetSetting(name, raw);
  return SettingsBody(runtime);
}

boost::json::value HandleApiResetSettings(Runtime& runtime, const boost::json::value&) {
  runtime.ClearSettings();
  return SettingsBody(runtime);
}

using Handler = boost::json::value (*)(Runtime&, const boost::json::value& body);
using serve::Board;
using serve::JobRequest;
using BoardHandler = boost::json::value (*)(Board&, const boost::json::value& body);

enum class Envelope { kObject, kArray, kAgents, kWorkspaces };

struct Route {
  std::string_view target;
  http::verb method;
  Handler handler;
  Envelope envelope;
  bool has_body;
};

struct BoardRoute {
  std::string_view target;
  http::verb method;
  BoardHandler handler;
  bool has_body;
  // A peer changes shared state when it writes, so the write routes present the token. A read
  // does too: reading is what tells a peer this one is still working, and that is not a claim
  // worth making about a server that cannot prove it holds the secret.
  bool authenticated;
  // The page reads the board too, from the machine the server runs on. It holds no token, so it
  // is served a read-only copy that says nothing about who is here and records nothing about it.
  bool local_only;
};

constexpr std::chrono::seconds kOfflineAfter{45};

// A board answer is always the job that resulted, plus the outcome when the move settled it.
boost::json::value RenderResult(const Board::ActionResult& result) {
  boost::json::object answer{{"success", true}, {"job", Board::Render(result.job)}};
  if (result.outcome) {
    boost::json::object outcome{{"id", result.outcome->id}, {"state", result.outcome->state}};
    if (result.outcome->result) outcome["result"] = *result.outcome->result;
    answer["outcome"] = std::move(outcome);
  }
  return answer;
}

// A pull is the liveness signal: it carries the token, and the board records the peer it names
// as heard from. Nothing is asked of the caller beyond the secret it already has.
boost::json::value HandleBoardGet(Board& board, const boost::json::value&) {
  return board.Snapshot(kOfflineAfter);
}

// What the page draws: the jobs, without the peers. Liveness is the one thing a local read
// cannot speak to, because being read by a page says nothing about a peer, and a reader told
// otherwise would take this machine's attention for a peer's.
boost::json::value HandleBoardPageGet(Board& board, const boost::json::value&) {
  boost::json::object page = board.Snapshot(kOfflineAfter).as_object();
  page.erase("peers");
  return page;
}

// The name a token speaks for. A token is one peer's, so a request carrying it is that peer.
std::string PeerForToken(Runtime& runtime, const std::string& token) {
  const auto& peers = runtime.Peers();
  if (!peers.has_value() || token.empty() || !peers->Accepts(token)) return {};
  return peers->self_name;
}

boost::json::value HandleBoardPost(Board& board, const boost::json::value& body) {
  JobRequest request;
  request.from = json::OptionalOrDefault<std::string>(body, "from");
  request.to = json::OptionalOrDefault<std::string>(body, "to");
  request.intent = json::OptionalOrDefault<std::string>(body, "intent");
  request.due = json::OptionalOrDefault<std::string>(body, "due");

  return RenderResult(board.Post(request));
}

boost::json::value HandleBoardPatch(Board& board, const boost::json::value& body) {
  JobRequest request;
  request.job = json::OptionalOrDefault<std::string>(body, "job");
  request.from = json::OptionalOrDefault<std::string>(body, "from");
  request.state = json::OptionalOrDefault<std::string>(body, "state");
  request.result = json::OptionalOrDefault<std::string>(body, "result");
  request.question = json::OptionalOrDefault<std::string>(body, "question");
  request.note = json::OptionalOrDefault<std::string>(body, "note");

  return RenderResult(board.Update(request));
}

boost::json::value HandleBoardAnswer(Board& board, const boost::json::value& body) {
  const std::string id = json::ValueOrDefault<std::string>(body, "job", "");
  const std::string answer = json::ValueOrDefault<std::string>(body, "answer", "");
  if (id.empty()) throw RequestRefused("answering a job needs the job's 'id'");
  if (answer.empty()) throw RequestRefused("an answer needs the 'answer' text to send back");

  return RenderResult(board.Answer(id, answer));
}

constexpr BoardRoute kBoardRoutes[] = {
    {"/api/jobs", http::verb::get, &HandleBoardGet, false, true, false},
    {"/api/jobs", http::verb::post, &HandleBoardPost, true, true, false},
    {"/api/jobs", http::verb::patch, &HandleBoardPatch, true, true, false},
    {"/api/jobs/answer", http::verb::post, &HandleBoardAnswer, true, true, false},
    {"/api/board/local", http::verb::get, &HandleBoardPageGet, false, false, true},
};

constexpr Route kRoutes[] = {
    {"/api/session", http::verb::get, &HandleApiSession, Envelope::kObject, false},
    {"/api/history", http::verb::get, &HandleApiHistory, Envelope::kArray, false},
    {"/api/agents", http::verb::get, &HandleApiAgents, Envelope::kAgents, false},
    {"/api/agent/switch", http::verb::post, &HandleApiAgentSwitch, Envelope::kObject, true},
    {"/api/clear", http::verb::post, &HandleApiClear, Envelope::kObject, false},
    {"/api/rewind", http::verb::post, &HandleApiRewind, Envelope::kObject, true},
    {"/api/settings", http::verb::get, &HandleApiSettings, Envelope::kObject, false},
    {"/api/settings", http::verb::post, &HandleApiSetSetting, Envelope::kObject, true},
    {"/api/settings/reset", http::verb::post, &HandleApiResetSettings, Envelope::kObject, false},
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

namespace {

// The bearer a peer presents, or empty when the header is absent or another scheme.
std::string BearerToken(const http::request<http::string_body>& req) {
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return {};
  const std::string_view value = it->value();
  constexpr std::string_view kPrefix = "Bearer ";
  if (value.size() <= kPrefix.size() || value.substr(0, kPrefix.size()) != kPrefix) return {};
  return std::string(value.substr(kPrefix.size()));
}

// An inbound message becomes a turn of its own: it is queued and answered 202, since the
// reply travels back through the sender's send_to_peer tool rather than this response. The
// answer names the turn and who took it, so the sender can watch the board for the outcome.
boost::json::value HandlePeerMessage(Runtime& runtime, SessionRunner* runner, Board* board,
                                     const http::request<http::string_body>& req) {
  const auto& peers = runtime.Peers();
  if (!peers.has_value()) throw NotFound("Peer messaging is not configured");
  if (!peers->Accepts(BearerToken(req))) throw RequestRefused("Invalid peer token");
  if (runner == nullptr) throw RuntimeError("This server cannot accept a peer turn");

  const boost::json::value body = ParseBody(req);
  const std::string sender = json::ValueOrDefault<std::string>(body, "from", "");
  const std::string message = json::ValueOrDefault<std::string>(body, "message", "");
  if (!peers->Knows(sender)) throw RequestRefused("Unknown peer: " + sender);
  if (message.empty()) throw RequestRefused("Missing or invalid 'message'");

  const std::string job_id = json::ValueOrDefault<std::string>(body, "job", "");
  if (!job_id.empty() && board != nullptr) {
    // Claiming before queueing is what makes a job that never runs visible as claimed rather
    // than silently open. A job already taken by someone else is a refusal, not a duplication.
    serve::JobRequest claim;
    claim.job = job_id;
    claim.from = peers->self_name;
    claim.state = std::string{serve::JobStateName(serve::JobState::kClaimed)};
    const serve::Board::ActionResult taken = board->Update(claim);
    spdlog::info("Peer '{}' job '{}' claimed", sender, job_id);
    if (taken.job.state != serve::JobState::kClaimed) {
      throw RequestRefused("job " + job_id + " did not end up claimed");
    }
  }

  const int hop = json::ValueOrDefault<int>(body, "hop", 0);
  spdlog::info("Peer '{}' sent a message at hop {}: {}", sender, hop, message.substr(0, 120));

  const std::string turn = runner->SubmitFromPeer(sender, message, job_id);
  const config::Backend backend = runtime.CurrentBackend();
  return boost::json::object{
      {"success", true},
      {"accepted", true},
      {"from", peers->self_name},
      {"turn", turn},
      {"agent", runtime.DefaultSession() ? runtime.DefaultSession()->Spec().agent_name
                                         : std::string()},
      {"model", backend.model},
  };
}

bool IsLoopback(const std::string& address) {
  return address.rfind("127.", 0) == 0 || address == "::1" || address == "::ffff:127.0.0.1";
}

}  // namespace

std::string PeerAddress(tcp::socket& socket, beast::error_code& ec) {
  const tcp::endpoint endpoint = socket.remote_endpoint(ec);
  return ec ? std::string{} : endpoint.address().to_string();
}

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex, SessionRunner* runner,
                         serve::Board* board, http::request<http::string_body>&& req,
                         http::response<http::string_body>& res, const std::string& peer) {
  const std::string_view target = req.target();

  if (IsPageFile(target)) {
    ServeFile(std::string(target), res);
    return;
  }

  for (const BoardRoute& route : kBoardRoutes) {
    if (target != route.target || req.method() != route.method) continue;
    if (board == nullptr) {
      SendError(res, 404, "This server has no board");
      return;
    }

    if (route.authenticated) {
      const std::string holder = PeerForToken(runtime, BearerToken(req));
      if (holder.empty()) {
        SendError(res, 403, "A board request carries a peer token");
        return;
      }
      // Reading is being heard from. Recorded before the snapshot so the answer includes it.
      board->TouchPeer(holder);
    }

    if (route.local_only && !IsLoopback(peer)) {
      SendError(res, 403, "The page's board is read on the machine the server runs on");
      return;
    }

    try {
      const boost::json::value body = route.has_body ? ParseBody(req) : boost::json::value{};
      SendJson(res, 200, route.handler(*board, body));
    } catch (const std::exception& e) {
      SendError(res, ErrorStatus(e), e.what());
    }
    return;
  }

  if (target == "/api/peer/message" && req.method() == http::verb::post) {
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      SendJson(res, 202, HandlePeerMessage(runtime, runner, board, req));
    } catch (const std::exception& e) {
      SendError(res, ErrorStatus(e), e.what());
    }
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
