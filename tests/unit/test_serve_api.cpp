// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "serve/serve_internal.hpp"
#include "pu/app/cli.hpp"
#include "pu/model/gateway.hpp"
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

// A directory this case creates beside the executable and removes on the way out, including when
// an assertion fails first. Without the destructor a failing run leaves `web/` behind and the
// next run fails for a reason that belongs to this one.
class BesideTheExecutable {
 public:
  explicit BesideTheExecutable(const fs::path& dir) : dir_(dir) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    failed_ = static_cast<bool>(ec);
  }
  ~BesideTheExecutable() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  BesideTheExecutable(const BesideTheExecutable&) = delete;
  BesideTheExecutable& operator=(const BesideTheExecutable&) = delete;
  bool failed() const { return failed_; }

 private:
  fs::path dir_;
  bool failed_ = false;
};

}  // namespace

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/session");
  auto j = ParseJson(body);

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("agent_name") == "chat");
  REQUIRE(j.at("gateway") == "ollama");
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
  ServeHarness harness("tencent");
  auto client = harness.Client();

  auto j = ParseJson(client.Get("/api/session"));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("gateway") == "tencent");
  REQUIRE(j.at("backend_model") == "test-model");
}

TEST_CASE("serve API /api/rewind steps back a turn the page asked", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "first");
  session->Messages().Append("assistant", "second");
  session->Messages().Append("user", "mine");

  const auto j = HistoryOf(client);
  REQUIRE(j.as_array().size() == 3);

  const auto rewind = client.PostFull("/api/rewind", boost::json::object{{"turn", 3}});
  CHECK(rewind.status == 200);
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
  // Not a page, so no envelope: absent is what every result said before the field existed.
  REQUIRE_FALSE(j.as_array()[2].as_object().contains("page"));
}

TEST_CASE("serve API /api/history replays a page as a page", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
  conversation.Append("user", "show the log");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{boost::json::object{
      {"id", "call_1"},
      {"type", "function"},
      {"function", boost::json::object{{"name", "execute_bash"},
                                       {"arguments", boost::json::object{{"command", "cat log"}}}}}}};
  conversation.Append(assistant);

  // The shape `execute_bash` writes for a run it had to page: the text, then the envelope
  // beside it. A reloaded turn has to draw this as a page rather than as a whole answer.
  pu::ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "execute_bash";
  receipt.content =
      R"({"success":true,"blocked":false,"output":"one\ntwo","error":"","exit_code":0,)"
      R"("lines":{"first":1,"last":2,"of":3500},)"
      R"("stored":{"printed":3500,"kept":80,"dropped":3420,"printed_bytes":200000,)"
      R"("kept_bytes":5000,"kept_from":"the end"},)"
      R"("buffer":{"id":"b1","source":"cat log","rank":0}})";
  conversation.Append(receipt);

  auto j = HistoryOf(client);
  const auto& turn = j.as_array()[2].as_object();
  REQUIRE(turn.at("output") == "one\ntwo");
  REQUIRE(turn.contains("page"));
  REQUIRE(turn.at("page").at("lines").at("of").as_int64() == 3500);
  REQUIRE(turn.at("page").at("stored").at("dropped").as_int64() == 3420);
}

TEST_CASE("serve API /api/backend says what is in force", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/backend")).at("backend");

  // The backend in force, from the agent file. The harness writes an ollama gateway by default;
  // the model is the one it wrote.
  REQUIRE(j.at("gateway") == "ollama");
  REQUIRE(j.at("model") == "test-model");
  // What it could be instead is not carried here: the gateways are the build's own table and the
  // models belong to one gateway, each asked at its own address.
  const auto& obj = j.as_object();
  REQUIRE(!obj.contains("gateways"));
  REQUIRE(!obj.contains("models"));
  REQUIRE(!obj.contains("model_error"));
  // The level travels with the backend the way the sampling settings do, named the way the page's
  // menu names it: a model that stated no level answers `auto`, not `default`, because that is the
  // word its form offers and the word a write accepts.
  CHECK(obj.at("thinking") == "auto");
}

// gateway that answers perfectly well.
TEST_CASE("serve API /api/models asks the configured address, not the gateway default", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/models?gateway=ollama"));

  // The harness wrote `ollama` with the fake backend's port as its address, while the gateway's
  // own row names 11434, where nothing is listening here.
  REQUIRE(j.at("error") == "");
  const auto& models = j.at("models");
  REQUIRE(models.as_array().size() == 2);
  REQUIRE(models.as_array()[0] == "test-model");
  REQUIRE(models.as_array()[1] == "second-model");
}

// The backend is written back to the agent file, and the address goes with the gateway and the
// model: a reader pointing a local gateway at a port the table has never heard of has to be able
// to put that address somewhere that survives the process.
namespace {
// What the agent file says about its backend, read from disk rather than from a reply: the
// question is whether the write landed, and the server's own answer cannot settle that.
boost::json::value BackendInFile(const fs::path& home) {
  std::ifstream file(home / ".pu" / "agents.json", std::ios::binary);
  REQUIRE(file.is_open());
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  INFO(text);
  // Parsed into a named value before it is looked into: `at` on a temporary would hand back a
  // reference into a document that dies at the end of the statement.
  const boost::json::value document = boost::json::parse(text);
  const boost::json::object& root = document.as_object();
  REQUIRE(root.contains("backend"));
  return root.at("backend");
}

}  // namespace

TEST_CASE("serve API /api/backend writes a thinking level into the agent file", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  for (const auto& [sent, expected] : std::vector<std::pair<std::string, std::string>>{
           {"none", "none"}, {"low", "low"}, {"medium", "medium"}, {"high", "high"}}) {
    const auto res = client.PostFull("/api/backend",
                                     boost::json::object{{"gateway", "ollama"},
                                                         {"model", "test-model"},
                                                         {"thinking", sent}});
    REQUIRE(res.status == 200);
    // The file is what outlives the process, so it is what the level has to reach: the reply and
    // the file agree, and a level this build does not know is refused rather than written.
    const boost::json::value written = BackendInFile(harness.Home());
    REQUIRE(written.is_object());
    CHECK(boost::json::value_to<std::string>(written.at("thinking")) == expected);
    // The reply is read through a named value too: `as_object()` on a returned temporary would
    // leave the reference it hands back pointing at a document that dies at the end of the
    // statement.
    const boost::json::value echoed = ParseJson(res.body);
    CHECK(boost::json::value_to<std::string>(echoed.at("backend").at("thinking")) == expected);
  }
}

TEST_CASE("serve API /api/backend writes auto as the word the loader reads", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  REQUIRE(client.PostFull("/api/backend", boost::json::object{{"gateway", "ollama"},
                                                             {"model", "test-model"},
                                                             {"thinking", "high"}})
              .status == 200);
  // `auto` is the absence of a level, and the file spells that absence `default`: the type cannot
  // hold a hole, so writing the word is what keeps "the gateway decides" a state the loader can
  // read back rather than a key it silently ignores.
  const auto res = client.PostFull("/api/backend", boost::json::object{{"gateway", "ollama"},
                                                                       {"model", "test-model"},
                                                                       {"thinking", "auto"}});
  REQUIRE(res.status == 200);
  const boost::json::value written = BackendInFile(harness.Home());
  REQUIRE(written.is_object());
  CHECK(boost::json::value_to<std::string>(written.at("thinking")) == "default");
  CHECK(boost::json::value_to<std::string>(
      ParseJson(res.body).at("backend").at("thinking")) == "auto");
}

TEST_CASE("serve API /api/backend refuses a thinking level it does not know", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();
  // The harness's own agent file need not name a level, so the "before" is read the way the
  // loader reads it: an absent key is `default`, not an error.
  const boost::json::value before_file = BackendInFile(harness.Home());
  REQUIRE(before_file.is_object());
  const std::string before =
      before_file.as_object().contains("thinking")
          ? boost::json::value_to<std::string>(before_file.at("thinking"))
          : std::string("default");

  const auto res = client.PostFull("/api/backend", boost::json::object{{"gateway", "ollama"},
                                                                       {"model", "test-model"},
                                                                       {"thinking", "loud"}});
  CHECK(res.status != 200);
  // A refused write changes nothing: the file still holds the level that was there.
  const boost::json::value after = BackendInFile(harness.Home());
  REQUIRE(after.is_object());
  const std::string now = after.as_object().contains("thinking")
                              ? boost::json::value_to<std::string>(after.at("thinking"))
                              : std::string("default");
  CHECK(now == before);
}

// `/api/models` asks the gateway the request names, and it must ask it at the address the agent
// file gives rather than at the one the build's gateway row names: a reader who pointed their
// gateway somewhere else would otherwise get an empty list after a connect timeout, for a
TEST_CASE("serve API /api/backend writes a typed host into the agent file", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto res = client.PostFull("/api/backend",
                                   boost::json::object{{"gateway", "ollama"},
                                                       {"model", "test-model"},
                                                       {"host", "http://10.0.0.7:11434"}});
  REQUIRE(res.status == 200);

  const boost::json::value& backend = BackendInFile(harness.Home());
  REQUIRE(backend.is_object());
  CHECK(boost::json::value_to<std::string>(backend.at("host")) == "http://10.0.0.7:11434");
  CHECK(boost::json::value_to<std::string>(backend.at("model")) == "test-model");
  // The reply is the same shape a read answers in, so the page draws what it just wrote.
  CHECK(boost::json::value_to<std::string>(ParseJson(res.body).at("backend").at("host")) ==
        "http://10.0.0.7:11434");
}

TEST_CASE("serve API /api/backend writes a placeholder host as spelled", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  // `${VAR}` is how a file names an environment rather than an address, so it is neither expanded
  // here nor refused by the scheme check: the next load is where it becomes a destination.
  const auto res = client.PostFull("/api/backend",
                                   boost::json::object{{"gateway", "openai"},
                                                       {"model", "test-model"},
                                                       {"host", "${OPENAI_BASE_URL}"}});
  REQUIRE(res.status == 200);
  // Read through a named value: `as_object()` on a returned temporary would leave the reference
  // it hands back pointing at a document that dies at the end of the statement.
  const boost::json::value written = BackendInFile(harness.Home());
  REQUIRE(written.is_object());
  CHECK(boost::json::value_to<std::string>(written.at("host")) == "${OPENAI_BASE_URL}");
}

TEST_CASE("serve API /api/backend emptied host returns the gateway's default", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  // A reader who clears the field is asking for the address the gateway comes with, not for an
  // empty string: the reply and the file both name the default, so the page can leave the field
  // showing what the gateway will actually use.
  const auto res = client.PostFull("/api/backend",
                                   boost::json::object{{"gateway", "ollama"},
                                                       {"model", "test-model"}, {"host", ""}});
  REQUIRE(res.status == 200);
  const boost::json::value& backend = BackendInFile(harness.Home());
  REQUIRE(backend.is_object());
  CHECK(boost::json::value_to<std::string>(backend.at("host")) ==
        pu::model::DefaultHostFor(pu::model::GatewayId::kOllama));
  CHECK(boost::json::value_to<std::string>(ParseJson(res.body).at("backend").at("host")) ==
        pu::model::DefaultHostFor(pu::model::GatewayId::kOllama));
}

TEST_CASE("serve API /api/backend refuses a host that is not a destination", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();
  const std::string written =
      boost::json::value_to<std::string>(BackendInFile(harness.Home()).as_object().at("host"));

  // No scheme: the address the page would build from it has none either, and the turn it broke
  // would fail a long way from the field that was typed in.
  const auto bare = client.PostFull("/api/backend",
                                    boost::json::object{{"gateway", "ollama"},
                                                        {"model", "test-model"},
                                                        {"host", "127.0.0.1:11434"}});
  CHECK(bare.status == 400);
  CHECK(ParseJson(bare.body).at("error") == "'host' must start with http:// or https://");

  const auto spaced = client.PostFull("/api/backend",
                                      boost::json::object{{"gateway", "ollama"},
                                                          {"model", "test-model"},
                                                          {"host", "http://1.2.3.4 :80"}});
  CHECK(spaced.status == 400);

  // Neither refusal wrote anything: the file still holds the address the harness gave it.
  CHECK(boost::json::value_to<std::string>(
            BackendInFile(harness.Home()).as_object().at("host")) == written);
}

TEST_CASE("serve API /api/backend keeps no address across a gateway change", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();
  REQUIRE(client.PostFull("/api/backend",
                          boost::json::object{{"gateway", "ollama"}, {"model", "test-model"},
                                              {"host", "http://10.0.0.7:11434"}}).status == 200);

  // One gateway's address means nothing to the next: a menu that swapped the scheme and kept the
  // port would point openai at ollama's server.
  const auto res = client.PostFull("/api/backend",
                                   boost::json::object{{"gateway", "openai"},
                                                       {"model", "test-model"}});
  REQUIRE(res.status == 200);
  const boost::json::value& backend = BackendInFile(harness.Home());
  REQUIRE(backend.is_object());
  CHECK(boost::json::value_to<std::string>(backend.at("gateway")) == "openai");
  CHECK(boost::json::value_to<std::string>(backend.at("host")) != "http://10.0.0.7:11434");
  CHECK(boost::json::value_to<std::string>(ParseJson(res.body).at("backend").at("host")) ==
        pu::model::DefaultHostFor(pu::model::GatewayId::kOpenAI));
}

TEST_CASE("serve API /api/gateways lists every gateway this build carries", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/gateways"));
  const auto& gateways = j.at("gateways").as_array();
  // The table itself, in its order: a build that lost one of these could not be told to use it.
  REQUIRE(gateways.size() >= 7);
  const std::vector<std::string> wanted = {"ollama", "ollama-cloud", "openai", "deepseek",
                                           "nvidia", "tencent", "anthropic"};
  for (const std::string& name : wanted) {
    bool saw = false;
    for (const auto& gateway : gateways) {
      if (gateway.as_string() == name) saw = true;
    }
    REQUIRE(saw);
  }
}

TEST_CASE("serve API /api/gateways says what each gateway's address is",
          "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const auto j = ParseJson(client.Get("/api/gateways"));
  REQUIRE(j.at("success") == true);
  const auto& gateways = j.at("gateways").as_array();
  const auto& hosts = j.at("hosts").as_object();
  // Every name in the list answers with an address, and it is the one the table would dial if the
  // file named none. The page puts this in the address box when the reader switches gateway, so a
  // missing or empty entry is a box left holding the address of the gateway they just left -- an
  // address for another vendor's endpoint, which the next Save would post to.
  CHECK(hosts.size() == gateways.size());
  for (const auto& gateway : gateways) {
    const std::string name = boost::json::value_to<std::string>(gateway);
    REQUIRE(hosts.contains(name));
    const std::string host = boost::json::value_to<std::string>(hosts.at(name));
    CHECK(!host.empty());
  }
  // One known answer, so the map is the table's and not a set of identical strings: `openai` is
  // the gateway the harness starts on, and its default is the one the routing layer would use.
  CHECK(boost::json::value_to<std::string>(hosts.at("openai")) ==
        pu::model::DefaultHostFor(pu::model::GatewayId::kOpenAI));
}

TEST_CASE("serve API /api/backend names the gateways when it refuses a name that is none",
          "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();
  const std::string written =
      boost::json::value_to<std::string>(BackendInFile(harness.Home()).as_object().at("host"));

  // The page lets a gateway be typed rather than picked, so the refusal is the only place a reader
  // is told what the list held. A bare "Unknown gateway: gpt" would leave them guessing at the
  // spelling of a name the menu cannot offer while they are typing in a box.
  const auto res = client.PostFull(
      "/api/backend", boost::json::object{{"gateway", "gpt"}, {"model", "test-model"}});
  REQUIRE(res.status == 400);
  const std::string error = boost::json::value_to<std::string>(ParseJson(res.body).at("error"));
  CHECK(error.find("Unknown gateway: gpt") != std::string::npos);
  for (const std::string& name : {"ollama", "openai", "deepseek", "anthropic"}) {
    CHECK(error.find(name) != std::string::npos);
  }
  // A refusal wrote nothing: the file still holds what the harness gave it.
  CHECK(boost::json::value_to<std::string>(
            BackendInFile(harness.Home()).as_object().at("host")) == written);
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

  auto res = client.PostRaw("/api/settings", "not json");

  REQUIRE(res.status == 400);
  auto j = ParseJson(res.body);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "Invalid JSON");
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
  const auto routes = pu::serve::detail::HttpRoutes();
  REQUIRE_FALSE(routes.empty());

  // The one route whose answer is a fact about this machine rather than about the session: the
  // path per server this machine is serving. Everything else changes or reads the session the
  // page is attached to, and answers whoever reaches the port. The bind address is the control
  // there, not a route.
  const std::set<std::string> kLoopbackOnly{"/api/servers"};

  for (const pu::serve::detail::Route& route : routes) {
    const std::string target{route.target};
    INFO(target);
    if (kLoopbackOnly.count(target) > 0) {
      CHECK(route.audience == pu::serve::detail::Audience::kLoopback);
      continue;
    }
    CHECK(route.audience == pu::serve::detail::Audience::kAnyone);
  }
}

TEST_CASE("an audience admits a caller by the two questions it can fail", "[serve][api][rules]") {
  using pu::serve::detail::Admission;
  using pu::serve::detail::Admit;
  using pu::serve::detail::Audience;

  // Anyone: the address is irrelevant.
  CHECK(Admit(Audience::kAnyone, "192.168.1.10") == Admission::kAllowed);
  CHECK(Admit(Audience::kAnyone, "") == Admission::kAllowed);

  // Loopback: only this machine's own addresses, and an address the socket could not name is not
  // one of them — refusing it is what keeps the rule true when the caller is unknown.
  CHECK(Admit(Audience::kLoopback, "127.0.0.1") == Admission::kAllowed);
  CHECK(Admit(Audience::kLoopback, "::1") == Admission::kAllowed);
  CHECK(Admit(Audience::kLoopback, "192.168.1.10") == Admission::kNotFromHere);
  CHECK(Admit(Audience::kLoopback, "") == Admission::kNotFromHere);
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
  write_page("panel.js", "const PANEL = true;");

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
  const HttpResponse panel = client.GetRaw("/panel.js");
  CHECK(panel.status == 200);
  CHECK(panel.body == "const PANEL = true;");

  const HttpResponse unmounted = client.GetRaw("/secret.txt");
  CHECK(unmounted.status == 404);

  fs::remove_all(web, ec);
}

// The other page test points PU_WEB_DIR at its fixtures, which means it never reaches the lookup
// the shipped binary actually uses. That lookup derives a path from the module handle, and a wrong
// answer there is silent: the assets simply 404 and the page comes up blank.
TEST_CASE("serve page files are found beside the executable", "[serve][page]") {
  const fs::path exe_dir = pu::serve::detail::ExecutableDir();
  REQUIRE_FALSE(exe_dir.empty());
  REQUIRE(fs::is_directory(exe_dir));

  const fs::path web = exe_dir / "web";
  std::error_code ec;
  // The directory is the fixture: whatever an interrupted earlier run left here is removed
  // first, so this case cannot fail for a reason that belongs to a previous one. `web` is
  // beside the executable rather than in the build tree, so a failure needs no cleanup of its
  // own to leave the checkout as it found it.
  fs::remove_all(web, ec);
  BesideTheExecutable beside(web);
  REQUIRE_FALSE(beside.failed());

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
}
