// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "app/serve_internal.hpp"
#include "pu/app/cli.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/serve_harness.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace fs = std::filesystem;

using pu::tests::HttpResponse;
using pu::tests::ScopedEnvVar;
using pu::tests::ServeHarness;
using pu::tests::TestHttpClient;

namespace {

boost::json::value ParseJson(const std::string& s) { return boost::json::parse(s); }

// `/api/history` answers the envelope every route uses: the payload under the key its row names,
// with `success` beside it. A case says which member it is reading, which is the point of the
// envelope — the shape is declared once, in the route table, rather than guessed per route.
boost::json::value HistoryOf(TestHttpClient& client) {
  return ParseJson(client.Get("/api/history")).at("messages");
}

}  // namespace

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/session");
  auto j = ParseJson(body);

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("agent_name") == "chat");
  REQUIRE(j.at("backend_type") == "ollama");
  REQUIRE(j.at("backend_model") == "test-model");
}

// The page draws a context bar against this figure, so an absent one has to be answered rather
// than left out: `0` means "this gateway states no window", which is what a blank bar reads as.
// The harness's fake backend serves no catalogue, so this is also the gateway-that-says-nothing
// case — the shape a NIM agent arrives in.
TEST_CASE("serve API /api/session says how much context the model holds", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/session"));

  REQUIRE(j.as_object().contains("context_window"));
  CHECK(j.at("context_window") == 0);
}

TEST_CASE("serve API /api/session names the backend it is talking to", "[serve][api]") {
  ServeHarness harness("codebuddy");
  auto client = harness.Client();

  auto j = ParseJson(client.Get("/api/session"));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("backend_type") == "codebuddy");
  REQUIRE(j.at("backend_model") == "test-model");
}

// A user message that arrived from a peer keeps the peer's name beside it, so a page says who
// asked rather than "You". The field is absent for a turn the page itself sent.
TEST_CASE("serve API /api/history names the peer a turn arrived from", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::ChatMessage from_peer;
  from_peer.role = "user";
  from_peer.content = "ping";
  from_peer.from_peer = "B";
  session->Messages().Append(from_peer);
  session->Messages().Append("assistant", "pong");
  session->Messages().Append("user", "mine");

  const auto j = HistoryOf(client);
  REQUIRE(j.as_array().size() == 3);
  CHECK(j.as_array()[0].at("from_peer") == "B");
  CHECK_FALSE(j.as_array()[2].as_object().contains("from_peer"));

  // A peer's turn is not this page's to take back, and the refusal says why.
  const auto rewind = client.PostFull("/api/rewind", boost::json::object{{"turn", 1}});
  CHECK(rewind.status == 400);
  CHECK(rewind.body.find("arrived from a peer") != std::string::npos);

  // A turn the page asked is still rewindable.
  const auto mine = client.PostFull("/api/rewind", boost::json::object{{"turn", 3}});
  CHECK(mine.status == 200);
}

TEST_CASE("serve API /api/history initially empty", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto j = HistoryOf(client);

  REQUIRE(j.is_array());
  REQUIRE(j.as_array().empty());
}

TEST_CASE("serve API /api/history says how each tool call ended", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "run it");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{
      boost::json::object{
          {"id", "call_1"},
          {"type", "function"},
          {"function",
           boost::json::object{{"name", "execute_bash"},
                               {"arguments", boost::json::object{{"command", "echo hi"}}}}}},
      boost::json::object{
          {"id", "call_2"},
          {"type", "function"},
          {"function",
           boost::json::object{{"name", "execute_bash"},
                               {"arguments", boost::json::object{{"command", "echo there"}}}}}},
  };
  conversation.Append(assistant);

  pu::ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "execute_bash";
  receipt.content = boost::json::serialize(boost::json::object{
      {"success", true}, {"blocked", false}, {"output", "hi\n"}, {"error", ""}, {"exit_code", 0}});
  conversation.Append(receipt);

  auto j = HistoryOf(client);
  REQUIRE(j.as_array().size() == 3);

  const boost::json::value& call_turn = j.as_array()[1];
  REQUIRE(call_turn.at("tool_calls").as_array().size() == 2);
  REQUIRE(call_turn.at("tool_call_status") == boost::json::array{"done", "pending"});

  const boost::json::value& result_turn = j.as_array()[2];
  REQUIRE(result_turn.at("output") == "hi\n");
  REQUIRE(result_turn.at("error") == "");
}

TEST_CASE("serve API /api/history names a call the turn never ran", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "run it");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{
      boost::json::object{
          {"id", "call_1"},
          {"type", "function"},
          {"function",
           boost::json::object{{"name", "execute_bash"},
                               {"arguments", boost::json::object{{"command", "echo hi"}}}}}},
  };
  conversation.Append(assistant);

  conversation.SealInterruptedToolCalls();

  auto j = HistoryOf(client);
  REQUIRE(j.as_array().size() == 3);
  REQUIRE(j.as_array()[1].at("tool_call_status") == boost::json::array{"interrupted"});
  REQUIRE(j.as_array()[2].at("role") == "tool");
}

TEST_CASE("serve API /api/history keeps a result it cannot parse", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "run it");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{boost::json::object{
      {"id", "call_1"},
      {"type", "function"},
      {"function", boost::json::object{{"name", "execute_bash"},
                                       {"arguments", boost::json::object{{"command", "ls"}}}}}}};
  conversation.Append(assistant);

  pu::ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "execute_bash";
  receipt.content = "plain text from a tool";
  conversation.Append(receipt);

  auto j = HistoryOf(client);
  REQUIRE(j.as_array().size() == 3);
  REQUIRE(j.as_array()[2].at("output") == "plain text from a tool");
  REQUIRE(j.as_array()[2].at("error") == "");
}

TEST_CASE("serve API /api/agents", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/agents");
  auto j = ParseJson(body);

  REQUIRE(j.at("agents").is_array());
  auto& agents = j.at("agents").as_array();
  REQUIRE(agents.size() == 1);
  REQUIRE(agents[0].at("name") == "chat");
}

TEST_CASE("serve API /api/settings names every setting and where it came from",
          "[serve][api]") {
  ServeHarness harness("openai");
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/settings"));
  REQUIRE(j.at("success") == true);
  const auto& settings = j.at("settings").as_array();
  REQUIRE(settings.size() == 4);

  bool saw_thinking = false;
  for (const auto& setting : settings) {
    const std::string name = boost::json::value_to<std::string>(setting.at("name"));
    CHECK(boost::json::value_to<std::string>(setting.at("source")) == "agent");
    if (name == "thinking") {
      saw_thinking = true;
      CHECK(setting.at("value") == "auto");
    } else if (name == "tool_budget") {
      CHECK(setting.at("value") == 100);  // the executor's default, which the agent did not name
    }
  }
  CHECK(saw_thinking);

  // The same answer, in the shape the transcript surface already reported.
  const auto session_j = ParseJson(client.Get("/api/session"));
  REQUIRE(session_j.at("settings").as_array().size() == 4);
}

TEST_CASE("serve API /api/settings accepts a value and reports it as the session's",
          "[serve][api]") {
  ServeHarness harness("openai");
  auto client = harness.Client();

  auto set_j = ParseJson(client.Post("/api/settings",
                                     boost::json::object{{"name", "thinking"}, {"value", "high"}}));
  REQUIRE(set_j.at("success") == true);
  bool thinking_from_session = false;
  for (const auto& setting : set_j.at("settings").as_array()) {
    if (setting.at("name") != "thinking") continue;
    thinking_from_session = true;
    CHECK(setting.at("value") == "high");
    CHECK(setting.at("source") == "session");
    CHECK(setting.at("agent_value") == "auto");
  }
  CHECK(thinking_from_session);

  const auto after = ParseJson(client.Get("/api/session"));
  REQUIRE(after.at("thinking") == "high");
  REQUIRE(after.at("thinking_override") == "high");

  // Clearing names the agent's value again, and the override field goes away.
  auto cleared =
      ParseJson(client.Post("/api/settings", boost::json::object{{"name", "thinking"}}));
  REQUIRE(cleared.at("success") == true);
  const auto back = ParseJson(client.Get("/api/session"));
  REQUIRE(back.at("thinking") == "default");
  REQUIRE_FALSE(back.as_object().count("thinking_override") == 1);
}

TEST_CASE("serve API /api/settings takes a budget, and reset drops every override",
          "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto set = client.PostFull(
      "/api/settings", boost::json::object{{"name", "tool_budget"}, {"value", 3}});
  REQUIRE(set.status == 200);
  REQUIRE(ParseJson(set.body).at("settings").as_array().size() == 4);

  const auto session_j = ParseJson(client.Get("/api/session"));
  bool budget_from_session = false;
  for (const auto& setting : session_j.at("settings").as_array()) {
    if (setting.at("name") != "tool_budget") continue;
    budget_from_session = true;
    CHECK(setting.at("value") == 3);
    CHECK(setting.at("source") == "session");
  }
  CHECK(budget_from_session);

  const auto reset = client.PostFull("/api/settings/reset", boost::json::object{});
  REQUIRE(reset.status == 200);
  // Parsed into a name first: the loop below binds a reference into the value, and a
  // temporary from a chained call would not outlive it.
  const auto reset_j = ParseJson(reset.body);
  for (const auto& setting : reset_j.at("settings").as_array()) {
    CHECK(boost::json::value_to<std::string>(setting.at("source")) == "agent");
  }
}

// The point of a session override is that the next turn runs under it, not that a number was
// echoed back: the budget in force is read from the runtime the turn uses.
TEST_CASE("a session's budget override is what the turn runs under", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  // The agent named no budget, so the executor's default is in force.
  REQUIRE(harness.RuntimeRef().Settings().size() == 4);
  int before = 0;
  for (const auto& view : harness.RuntimeRef().Settings()) {
    if (view.name == "tool_budget") before = static_cast<int>(view.value.as_int64());
  }
  REQUIRE(before == pu::Executor::kDefaultToolCallBudget);

  REQUIRE(client
              .PostFull("/api/settings",
                        boost::json::object{{"name", "tool_budget"}, {"value", 3}})
              .status == 200);

  for (const auto& view : harness.RuntimeRef().Settings()) {
    if (view.name != "tool_budget") continue;
    CHECK(static_cast<int>(view.value.as_int64()) == 3);
    CHECK(view.source == "session");
  }

  // Clearing it names the agent's number again, which is what a reset does for every row.
  REQUIRE(client.PostFull("/api/settings/reset", boost::json::object{}).status == 200);
  for (const auto& view : harness.RuntimeRef().Settings()) {
    if (view.name != "tool_budget") continue;
    CHECK(view.source == "agent");
    CHECK(static_cast<int>(view.value.as_int64()) == pu::Executor::kDefaultToolCallBudget);
  }
}

TEST_CASE("serve API /api/settings refuses a value the setting cannot take", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto unknown = client.PostFull(
      "/api/settings", boost::json::object{{"name", "sandbox_root"}, {"value", "/"}});
  REQUIRE(unknown.status == 400);
  CHECK(unknown.body.find("Unknown setting") != std::string::npos);

  const auto not_a_number = client.PostFull(
      "/api/settings", boost::json::object{{"name", "temperature"}, {"value", "hot"}});
  REQUIRE(not_a_number.status == 400);
  CHECK(not_a_number.body.find("must be a number") != std::string::npos);

  const auto out_of_range = client.PostFull(
      "/api/settings", boost::json::object{{"name", "temperature"}, {"value", 3.0}});
  REQUIRE(out_of_range.status == 400);
  CHECK(out_of_range.body.find("between 0 and 2") != std::string::npos);

  // Zero is not "unlimited": it would stop the turn at its first call.
  const auto zero =
      client.PostFull("/api/settings", boost::json::object{{"name", "tool_budget"}, {"value", 0}});
  REQUIRE(zero.status == 400);

  const auto missing = client.PostFull("/api/settings", boost::json::object{});
  REQUIRE(missing.status == 400);
  CHECK(missing.body.find("Missing or invalid 'name'") != std::string::npos);

  const auto malformed = client.PostRaw("/api/settings", "not json");
  REQUIRE(malformed.status == 400);
}

TEST_CASE("serve API invalid JSON returns 400", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto res = client.PostRaw("/api/agent/switch", "not json");

  REQUIRE(res.status == 400);
  auto j = ParseJson(res.body);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "Invalid JSON");
}

TEST_CASE("serve API switching to an agent that does not exist returns 404", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto res = client.PostFull("/api/agent/switch", boost::json::object{{"agent_name", "ghost"}});

  REQUIRE(res.status == 404);
  auto j = ParseJson(res.body);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "Agent not found: ghost");
}

TEST_CASE("serve API /api/rewind steps the session back", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "one");
  conversation.Append("assistant", "two");
  conversation.Append("user", "three");

  auto stepped = client.PostFull("/api/rewind", boost::json::object{{"turn", 3}});
  REQUIRE(stepped.status == 200);
  REQUIRE(ParseJson(stepped.body).at("success") == true);

  auto history = HistoryOf(client);
  REQUIRE(history.as_array().size() == 2);
  REQUIRE(history.as_array()[1].at("content") == "two");
}

TEST_CASE("serve API /api/rewind refuses a turn that is not there", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "one");

  auto missing = client.PostFull("/api/rewind", boost::json::object{});
  REQUIRE(missing.status == 400);
  REQUIRE(ParseJson(missing.body).at("error") == "Missing or invalid 'turn'");

  auto past_end = client.PostFull("/api/rewind", boost::json::object{{"turn", 9}});
  REQUIRE(past_end.status == 400);
  REQUIRE(ParseJson(past_end.body).at("error") == "No such turn");

  auto malformed = client.PostRaw("/api/rewind", "not json");
  REQUIRE(malformed.status == 400);
  REQUIRE(ParseJson(malformed.body).at("error") == "Invalid JSON");

  REQUIRE(HistoryOf(client).as_array().size() == 1);
}

TEST_CASE("serve API /api/rewind refuses while a tool call is pending", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "one");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  conversation.Append(assistant);
  REQUIRE(conversation.HasPendingToolCalls());

  auto refused = client.PostFull("/api/rewind", boost::json::object{{"turn", 1}});

  REQUIRE(refused.status == 400);
  REQUIRE(ParseJson(refused.body).at("success") == false);
}

// Every route states who may read it, and the dispatcher has no path names of its own. Asking
// the table is therefore asking the whole rule: a row added without an audience does not compile,
// and a row whose audience changed shows up here as a different answer rather than as a sentence
// in a document nobody re-reads.
TEST_CASE("every route states the audience it answers, and none of them silently widened",
          "[serve][api][rules]") {
  const auto routes = pu::cli::detail::HttpRoutes();
  REQUIRE_FALSE(routes.empty());

  // The routes whose answer is a fact about this machine rather than about the session: a path
  // per server and the trail of who asked whom for what.
  const std::map<std::string, pu::cli::detail::Audience> kLoopbackOnly{
      {"/api/servers", pu::cli::detail::Audience::kLoopback},
      {"/api/board", pu::cli::detail::Audience::kLoopback},
  };
  // The peer endpoint and the board verbs are the only surface that lets a remote caller change
  // something, and every one of them takes the shared token.
  const std::set<std::string> kPeerOnly{
      "/api/jobs", "/api/jobs/answer", "/api/jobs/chase", "/api/jobs/drop", "/api/peer/message",
  };

  for (const pu::cli::detail::Route& route : routes) {
    const std::string target{route.target};
    if (const auto it = kLoopbackOnly.find(target); it != kLoopbackOnly.end()) {
      INFO(target);
      CHECK(route.audience == it->second);
      continue;
    }
    if (kPeerOnly.count(target) > 0) {
      INFO(target);
      CHECK(route.audience == pu::cli::detail::Audience::kPeerToken);
      continue;
    }
    // Everything else is a route that changes or reads the session the page is attached to, and
    // it answers whoever reaches the port. The bind address is the control there, not a route.
    INFO(target);
    CHECK(route.audience == pu::cli::detail::Audience::kAnyone);
  }
}

TEST_CASE("an audience admits a caller by the three questions it can fail", "[serve][api][rules]") {
  using pu::cli::detail::Admission;
  using pu::cli::detail::Admit;
  using pu::cli::detail::Audience;

  pu::config::PeerConfig peers;
  peers.self_name = "A";
  peers.token = "shared-secret";
  const std::optional<pu::config::PeerConfig> configured = peers;

  // Anyone: the address and the token are both irrelevant.
  CHECK(Admit(Audience::kAnyone, "192.168.1.10", "", configured) == Admission::kAllowed);
  CHECK(Admit(Audience::kAnyone, "", "", std::nullopt) == Admission::kAllowed);

  // Loopback: only this machine's own addresses, and an address the socket could not name is not
  // one of them — refusing it is what keeps the rule true when the peer is unknown.
  CHECK(Admit(Audience::kLoopback, "127.0.0.1", "", std::nullopt) == Admission::kAllowed);
  CHECK(Admit(Audience::kLoopback, "::1", "", std::nullopt) == Admission::kAllowed);
  CHECK(Admit(Audience::kLoopback, "192.168.1.10", "", std::nullopt) ==
        Admission::kNotFromHere);
  CHECK(Admit(Audience::kLoopback, "", "", std::nullopt) == Admission::kNotFromHere);

  // The token: a server with no topology cannot accept a peer at all, which is a different answer
  // from a caller that presented the wrong secret.
  CHECK(Admit(Audience::kPeerToken, "10.0.0.5", "shared-secret", std::nullopt) ==
        Admission::kNoTopology);
  CHECK(Admit(Audience::kPeerToken, "10.0.0.5", "", configured) == Admission::kWrongToken);
  CHECK(Admit(Audience::kPeerToken, "10.0.0.5", "not-the-token", configured) ==
        Admission::kWrongToken);
  CHECK(Admit(Audience::kPeerToken, "10.0.0.5", "shared-secret", configured) ==
        Admission::kAllowed);
  // A peer may arrive from anywhere; the token is the whole credential, so the address is not
  // consulted. Pinned because a loopback check added here would look like a tightening and would
  // break every topology spread across two machines.
  CHECK(Admit(Audience::kPeerToken, "127.0.0.1", "shared-secret", configured) ==
        Admission::kAllowed);
}

TEST_CASE("serve API /api/servers lists the others and not this one", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto body = ParseJson(client.Get("/api/servers"));
  REQUIRE(body.at("success") == true);
  const auto& servers = body.at("servers").as_array();

  // The server that answered is the page's own; the list is what it may switch to, so this one is
  // not in it. A case that asserted otherwise would pin a control that offers a move to nowhere.
  for (const boost::json::value& entry : servers) {
    const boost::json::object& row = entry.as_object();
    CHECK(boost::json::value_to<int>(row.at("port")) != harness.Port());
    CHECK(row.contains("name"));
    // A path would tell a remote reader where this machine keeps its projects, which is why the
    // route is loopback-only and why the entry carries a label instead.
    CHECK_FALSE(row.contains("directory"));
  }
}

TEST_CASE("serve page files are mounted and served from the web directory", "[serve][page]") {
  const fs::path web = fs::temp_directory_path() / "pu_serve_web_fixture";
  std::error_code ec;
  fs::remove_all(web, ec);
  fs::create_directories(web, ec);
  REQUIRE_FALSE(ec);

  const auto write_page = [&](const std::string& name, const std::string& body) {
    std::ofstream file(web / name, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << body;
    REQUIRE(file.good());
  };
  write_page("index.html", "<!doctype html>fixture index");
  write_page("style.css", ":root{--column:720px}");
  write_page("app.js", "const APP = true;");
  write_page("board.js", "const BOARD = true;");

  const ScopedEnvVar web_dir("PU_WEB_DIR", web.string());

  ServeHarness harness;
  auto client = harness.Client();

  const HttpResponse root = client.GetRaw("/");
  CHECK(root.status == 200);
  CHECK(root.body == "<!doctype html>fixture index");

  const HttpResponse explicit_index = client.GetRaw("/index.html");
  CHECK(explicit_index.status == 200);
  CHECK(explicit_index.body == "<!doctype html>fixture index");

  const HttpResponse css = client.GetRaw("/style.css");
  CHECK(css.status == 200);
  CHECK(css.body == ":root{--column:720px}");

  const HttpResponse script = client.GetRaw("/app.js");
  CHECK(script.status == 200);
  CHECK(script.body == "const APP = true;");

  // The page's script list is a whitelist, not a directory read: a module added to
  // index.html but missed there is served as a 404 and the page silently loses it.
  const HttpResponse board = client.GetRaw("/board.js");
  CHECK(board.status == 200);
  CHECK(board.body == "const BOARD = true;");

  const HttpResponse unmounted = client.GetRaw("/secret.txt");
  CHECK(unmounted.status == 404);

  fs::remove_all(web, ec);
}

// The other page test points PU_WEB_DIR at its fixtures, which means it never reaches the lookup
// the shipped binary actually uses. That lookup derives a path from the module handle, and a wrong
// answer there is silent: the assets simply 404 and the page comes up blank.
TEST_CASE("serve page files are found beside the executable", "[serve][page]") {
  const fs::path exe_dir = pu::cli::detail::ExecutableDir();
  REQUIRE_FALSE(exe_dir.empty());
  REQUIRE(fs::is_directory(exe_dir));

  const fs::path web = exe_dir / "web";
  std::error_code ec;
  const bool preexisting = fs::exists(web, ec);
  REQUIRE_FALSE(preexisting);
  fs::create_directories(web, ec);
  REQUIRE_FALSE(ec);

  {
    std::ofstream file(web / "index.html", std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << "<!doctype html>beside the executable";
    REQUIRE(file.good());
  }

  const ScopedEnvVar unset_web_dir("PU_WEB_DIR", "");

  ServeHarness harness;
  auto client = harness.Client();

  const HttpResponse root = client.GetRaw("/");
  CHECK(root.status == 200);
  CHECK(root.body == "<!doctype html>beside the executable");

  fs::remove_all(web, ec);
}
