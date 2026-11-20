// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/agents.hpp"
#include "pu/config/setup.hpp"
#include "pu/model/backend.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/llm/model_factory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <filesystem>

using namespace pu;
using namespace pu::tests;

namespace fs = std::filesystem;

struct TempConfigFile {
  fs::path path;
  TempConfigFile() { path = UniqueTempPath("pu_test_agents").string() + ".json"; }
  ~TempConfigFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void write(const std::string& content) {
    std::ofstream file(path);
    file << content;
  }
};

namespace {

void WriteAgentsConfigForTest(const std::string& config_path, const config::AgentsConfig& cfg) {
  json::value j = {{"default_agent", cfg.default_agent}};

  json::array agents_array;
  for (const auto& entry : cfg.agents) {
    json::value item = {
        {"name", entry.name},
        {"description", entry.description},
    };
    json::value security = {
        {"sandbox_root", entry.security.sandbox_root},
        {"max_command_length", entry.security.max_command_length},
    };
    security.as_object()["forbidden_patterns"] =
        boost::json::value_from(entry.security.forbidden_patterns);
    item.as_object()["security"] = security;

    json::value backend = {
        {"gateway", (entry.backend.gateway == model::GatewayId::kOpenAI) ? "openai" : "ollama"},
        {"host", entry.backend.host},
        {"model", entry.backend.model},
        {"temperature", entry.backend.temperature},
        {"thinking", ThinkingLevelName(entry.backend.thinking)},
    };
    if (entry.backend.api_key) backend.as_object()["api_key"] = *entry.backend.api_key;
    if (entry.backend.context_window) {
      backend.as_object()["context_window"] = *entry.backend.context_window;
    }
    if (entry.backend.system_prompt)
      backend.as_object()["system_prompt"] = *entry.backend.system_prompt;
    item.as_object()["backend"] = backend;

    if (!entry.mcp_servers.empty()) {
      json::array mcp_array;
      for (const auto& srv : entry.mcp_servers) {
        json::value srv_json = {
            {"name", srv.name},
            {"command", srv.command},
        };
        srv_json.as_object()["args"] = boost::json::value_from(srv.args);
        if (!srv.url.empty()) srv_json.as_object()["url"] = srv.url;
        if (!srv.headers.empty()) {
          json::value headers = json::object{};
          for (const auto& [k, v] : srv.headers) headers.as_object()[k] = v;
          srv_json.as_object()["headers"] = headers;
        }
        mcp_array.push_back(srv_json);
      }
      item.as_object()["mcp_servers"] = mcp_array;
    }

    agents_array.push_back(item);
  }
  j.as_object()["agents"] = agents_array;

  std::ofstream file(config_path);
  if (!file.is_open()) throw pu::GenericError("Failed to open config file for writing: " + config_path);
  file << json::PrettyPrint(j);
}

}  // namespace

TEST_CASE("FindConfigPath prefers ./.pu/agents.json", "[agent_config]") {
  auto dir = UniqueTempPath("pu_findconfig_project");
  auto home = UniqueTempPath("pu_findconfig_home");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir / ".pu");
  fs::create_directories(home);

  auto old = fs::current_path();
  fs::current_path(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    std::ofstream f(dir / ".pu" / "agents.json");
    f << "{}";

    REQUIRE(config::FindConfigPath() == "./.pu/agents.json");
  }

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

TEST_CASE("FindConfigPath falls back to ~/.pu/agents.json", "[agent_config]") {
  auto dir = UniqueTempPath("pu_findconfig_project2");
  auto home = UniqueTempPath("pu_findconfig_home2");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir);
  fs::create_directories(home / ".pu");

  auto old = fs::current_path();
  fs::current_path(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    std::ofstream f(home / ".pu" / "agents.json");
    f << "{}";

    REQUIRE(config::FindConfigPath() == (home / ".pu" / "agents.json").string());
  }

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

TEST_CASE("FindConfigPath reports no configuration when neither location exists", "[agent_config]") {
  auto dir = UniqueTempPath("pu_findconfig_empty");
  auto home = UniqueTempPath("pu_findconfig_empty_home");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir);
  fs::create_directories(home);

  auto old = fs::current_path();
  fs::current_path(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    REQUIRE(config::FindConfigPath().empty());
  }

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

TEST_CASE("FindServeOptions reads the workspace's own bind and port", "[agent_config]") {
  auto dir = UniqueTempPath("pu_serve_options");
  auto home = UniqueTempPath("pu_serve_options_home");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir / ".pu");
  fs::create_directories(home);

  auto old = fs::current_path();
  fs::current_path(dir);

  {
    ScopedEnvVar env("HOME", home.string());

    {
      std::ofstream f(dir / ".pu" / "agents.json");
      f << R"({"default_agent":"a","agents":[]})";
    }
    REQUIRE_FALSE(config::FindServeOptions().has_value());

    {
      ScopedEnvVar bind_env("PU_TEST_SERVE_BIND", "127.0.0.2");
      {
        std::ofstream f(dir / ".pu" / "agents.json");
        f << R"({"serve":{"bind":"${PU_TEST_SERVE_BIND}","port":8087}})";
      }
      const auto options = config::FindServeOptions();
      REQUIRE(options.has_value());
      REQUIRE(options->port == 8087);
      REQUIRE(options->bind == "127.0.0.2");
    }

    {
      std::ofstream f(dir / ".pu" / "agents.json");
      f << R"({"serve":{"port":70000}})";
      f.close();
      REQUIRE_FALSE(config::FindServeOptions().has_value());
    }

    {
      std::ofstream f(dir / ".pu" / "agents.json");
      f << "{ not json";
      f.close();
      REQUIRE_FALSE(config::FindServeOptions().has_value());
    }
  }

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

// A file that describes one agent has nothing to choose between and nothing to default to,
// so it says neither: the name is at the top and the backend beneath it, which is the whole
// of what a workspace with one agent has to record.
TEST_CASE("A file describing one agent needs no default_agent and no agents array",
          "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "name": "solo",
    "backend": { "gateway": "ollama", "model": "qwen2.5:0.5b" }
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.default_agent == "solo");
  REQUIRE(cfg.agents.size() == 1);
  REQUIRE(cfg.agents[0].name == "solo");
  REQUIRE(cfg.agents[0].backend.gateway == model::GatewayId::kOllama);
  REQUIRE(cfg.agents[0].backend.model == "qwen2.5:0.5b");
  // The host was left out, so it is the gateway's own rather than empty: a backend with no
  // address named the one the gateway always has.
  REQUIRE(cfg.agents[0].backend.host == model::DefaultHostFor(model::GatewayId::kOllama));
}

TEST_CASE("A single-agent file that does not name itself is refused", "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  // There is no directory and no table to supply the name, and a name is what a session
  // records, so nobody but the file's author can answer it.
  REQUIRE_THROWS_WITH(config::LoadAgentsConfig(tmp.path.string()),
                      Catch::Matchers::ContainsSubstring("must name itself"));
}

TEST_CASE("A file with neither an agents array nor a backend is a table missing its array",
          "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({ "default_agent": "chat" })");

  // The table form's own error rather than this one's: a file that omits both keys is far
  // more likely a table that lost its array than a single agent that lost its backend.
  REQUIRE_THROWS_WITH(config::LoadAgentsConfig(tmp.path.string()),
                      Catch::Matchers::ContainsSubstring("Missing agents array"));
}

TEST_CASE("A backend that names no key takes the gateway's own variable", "[agent_config][single]") {
  TempConfigFile tmp;
  // No `api_key` at all, which is the ordinary case: the key is a secret in the environment
  // and writing the variable's name in every entry asks the file to restate what the gateway
  // already knows.
  tmp.write(R"({
    "name": "solo",
    "backend": { "gateway": "tencent", "model": "deepseek-v4.1-flash" }
  })");

  SECTION("set") {
    ScopedEnvVar env("TENCENT_API_KEY", "sk-from-the-environment");
    const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
    REQUIRE(cfg.agents.at(0).backend.api_key.has_value());
    REQUIRE(*cfg.agents.at(0).backend.api_key == "sk-from-the-environment");
  }

  SECTION("unset") {
    // Nothing to take, so nothing is invented: the backend has no key and the first turn
    // says so, rather than carrying an empty one that reads as a key which is wrong.
    ScopedEnvVar env("TENCENT_API_KEY", "");
    const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
    REQUIRE(!cfg.agents.at(0).backend.api_key.has_value());
  }
}

TEST_CASE("A key the file names is not replaced by the environment", "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "name": "solo",
    "backend": { "gateway": "tencent", "model": "m", "api_key": "${MY_OWN_KEY}" }
  })");

  ScopedEnvVar own("MY_OWN_KEY", "from-the-file");
  ScopedEnvVar gateway("TENCENT_API_KEY", "from-the-gateway");

  // The file's own answer, even though the gateway has one: a written key is the author's
  // choice, and falling back past it would silently use an account they did not name.
  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents.at(0).backend.api_key.has_value());
  REQUIRE(*cfg.agents.at(0).backend.api_key == "from-the-file");
}

TEST_CASE("An explicit empty key stays empty rather than falling back", "[agent_config][single]") {
  TempConfigFile tmp;
  // A variable that is not set: the expansion is empty, and that empty key is the author's
  // answer. Taking the gateway's variable instead would paper over a typo in the name they
  // wrote, which is the mistake this leaves visible.
  tmp.write(R"({
    "name": "solo",
    "backend": { "gateway": "tencent", "model": "m", "api_key": "${NOT_SET_ANYWHERE}" }
  })");

  ScopedEnvVar gateway("TENCENT_API_KEY", "from-the-gateway");
  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  // A key that expanded to nothing keeps the empty answer the author wrote rather than the
  // gateway's variable: the variable they named is the one they meant, and taking another one
  // would hide the typo. `pu check` reports this as "(empty)" with a warning.
  REQUIRE(cfg.agents.at(0).backend.api_key.has_value());
  REQUIRE(cfg.agents.at(0).backend.api_key->empty());
}

TEST_CASE("A blank key is accepted when its variable holds one", "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kCodeBuddy;
  choice.model = "m";
  // The question offered this variable as the default, so a blank answer is the intended one;
  // refusing it would make the prompt's own instruction unusable.
  ScopedEnvVar env("TENCENT_API_KEY", "sk-from-the-environment");
  REQUIRE_NOTHROW(config::ValidateSetupChoice(choice));
  // And the file names no key, leaving the run to read the variable -- writing it would copy a
  // secret into the workspace for no reason.
  const std::string rendered = config::RenderSetupConfig(choice);
  const boost::json::value parsed = boost::json::parse(rendered);
  REQUIRE(!parsed.at("backend").as_object().contains("api_key"));
}

TEST_CASE("A blank key is refused when its variable is unset", "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kCodeBuddy;
  choice.model = "m";
  // Nothing would answer the question, so the turn would fail on its first request somewhere
  // far from this prompt.
  ScopedEnvVar env("TENCENT_API_KEY", "");
  REQUIRE_THROWS_AS(config::ValidateSetupChoice(choice), GenericError);
}

TEST_CASE("RenderSetupConfig writes the single-agent form with no key when there is none",
          "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kOllama;
  choice.host = "http://localhost:9999";
  choice.model = "qwen2.5:0.5b";
  choice.agent_name = "default";

  const std::string text = config::RenderSetupConfig(choice);
  const json::value parsed = json::parse(text);

  REQUIRE(text.back() == '\n');
  REQUIRE(!json::HasKey(parsed, "default_agent"));
  REQUIRE(!json::HasKey(parsed, "agents"));
  REQUIRE(boost::json::value_to<std::string>(parsed.at("name")) == "default");
  const auto& backend = parsed.at("backend").as_object();
  REQUIRE(boost::json::value_to<std::string>(backend.at("gateway")) == "ollama");
  REQUIRE(boost::json::value_to<std::string>(backend.at("host")) == "http://localhost:9999");
  REQUIRE(boost::json::value_to<std::string>(backend.at("model")) == "qwen2.5:0.5b");
  // Absent rather than empty: a file that said `api_key: ""` would read as a key that is
  // wrong, where saying nothing reads as a product that never asked for one.
  REQUIRE(backend.find("api_key") == backend.end());
}

TEST_CASE("A gateway that reads a key is refused when the choice has none", "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kOpenAI;
  choice.model = "gpt-4o-mini";

  // A file written without it would fail on the first turn with a message about a header,
  // far from the question that was left blank.
  REQUIRE_THROWS_WITH(config::ValidateSetupChoice(choice),
                      Catch::Matchers::ContainsSubstring("enter one here"));

  // With a key it passes, which is the same check saying yes rather than a second test.
  choice.api_key = "sk-test";
  REQUIRE_NOTHROW(config::ValidateSetupChoice(choice));
}

TEST_CASE("A choice with no model is refused", "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kOllama;
  choice.model = "   ";

  REQUIRE_THROWS_WITH(config::ValidateSetupChoice(choice),
                      Catch::Matchers::ContainsSubstring("needs a model"));
}

TEST_CASE("A choice with no agent name is refused", "[setup]") {
  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kOllama;
  choice.model = "m";
  choice.agent_name = " ";

  REQUIRE_THROWS_WITH(config::ValidateSetupChoice(choice),
                      Catch::Matchers::ContainsSubstring("needs a name"));
}

TEST_CASE("WriteSetupConfig refuses to overwrite a file that is already there", "[setup]") {
  // `<workspace>/.pu/agents.json` is where the writer looks, so the workspace is built first
  // and the file placed where it would have gone. A run reaching this point found no file,
  // so one being there means something else wrote it and it is not this writer's to replace.
  const fs::path workspace = UniqueTempPath("pu_setup_ws");
  std::error_code ec;
  fs::create_directories(workspace / ".pu");
  {
    std::ofstream file(workspace / ".pu" / "agents.json", std::ios::trunc);
    file << R"({ "name": "kept", "backend": { "gateway": "ollama", "model": "m" } })";
  }

  config::SetupChoice choice;
  choice.gateway = model::GatewayId::kOllama;
  choice.model = "other";

  REQUIRE_THROWS_WITH(config::WriteSetupConfig(choice, workspace.string()),
                      Catch::Matchers::ContainsSubstring("already there"));
  // The file that was there is the one still there.
  const config::AgentsConfig kept =
      config::LoadAgentsConfig((workspace / ".pu" / "agents.json").string());
  REQUIRE(kept.agents.at(0).backend.model == "m");

  fs::remove_all(workspace, ec);
}

TEST_CASE("LoadAgentsConfig parses valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "type": "llm",
        "description": "Local Chat",
        "backend": {
          "gateway": "ollama",
          "host": "http://localhost:11434",
          "model": "qwen3.5:4b",
          "temperature": 0.7
        }
      },
      {
        "name": "bash",
        "type": "llm",
        "description": "Command Runner",
        "backend": {
          "gateway": "openai",
          "host": "https://api.openai.com/v1",
          "api_key": "${OPENAI_KEY}",
          "model": "gpt-4o-mini"
        },
        "security": {
          "sandbox_root": "/tmp"
        }
      }
    ]
  })";
  tmp.write(json);

  {
    ScopedEnvVar env("OPENAI_KEY", "test-key-123");
    config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());

    REQUIRE(cfg.default_agent == "chat");
    REQUIRE(cfg.agents.size() == 2);
    REQUIRE(cfg.agents[0].name == "chat");
    REQUIRE(cfg.agents[0].backend.gateway == model::GatewayId::kOllama);
    REQUIRE(cfg.agents[1].name == "bash");
    REQUIRE(cfg.agents[1].backend.gateway == model::GatewayId::kOpenAI);
    REQUIRE(cfg.agents[1].backend.api_key == "test-key-123");
    REQUIRE(cfg.agents[1].security.sandbox_root == "/tmp");
  }
}


namespace {

// A workspace of the shape this build reads: a top-level table, and one directory per agent
// beside it. Built and removed per case so two cases cannot see each other's files.
struct TempAgentWorkspace {
  fs::path root;
  TempAgentWorkspace() {
    root = UniqueTempPath("pu_agents_dir");
    fs::create_directories(root / ".pu");
  }
  ~TempAgentWorkspace() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path Table() const { return root / ".pu" / "agents.json"; }
  fs::path AgentDir(const std::string& name) const { return root / ".pu" / "agents" / name; }
  void WriteTable(const std::string& content) const {
    std::ofstream file(Table(), std::ios::trunc);
    file << content;
  }
  void WriteAgent(const std::string& name, const std::string& content) const {
    fs::create_directories(AgentDir(name));
    std::ofstream file(AgentDir(name) / "agents.json", std::ios::trunc);
    file << content;
  }
  void WritePersona(const std::string& name, const std::string& text) const {
    fs::create_directories(AgentDir(name));
    std::ofstream file(AgentDir(name) / "persona.md", std::ios::trunc);
    file << text;
  }
};

const char* kInlineOnly = R"({
  "default_agent": "chat",
  "agents": [
    { "name": "chat", "backend": { "gateway": "ollama", "host": "http://x:1", "model": "m" } }
  ]
})";

const char* kOneAgentDir = R"({
  "description": "reads and reports",
  "delegates": ["reviewer"],
  "tools": ["bash"],
  "backend": { "gateway": "ollama", "model": "m" }
})";

}  // namespace

TEST_CASE("An agent filed under .pu/agents/ is read by the name of its directory",
          "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reader", kOneAgentDir);

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());

  REQUIRE(cfg.agents.size() == 2);
  const auto& reader = cfg.agents[1];
  CHECK(reader.name == "reader");
  CHECK(reader.description == "reads and reports");
  CHECK(reader.delegates == std::vector<std::string>{"reviewer"});
  CHECK(reader.tools == std::vector<std::string>{"bash"});
  // A gateway with a default host supplies it, exactly as it does for an inline entry.
  CHECK_FALSE(reader.backend.host.empty());
  CHECK(reader.source_dir == ws.AgentDir("reader"));
}

TEST_CASE("A persona file becomes the agent's system prompt", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reviewer", R"({
    "persona": "persona.md",
    "backend": { "gateway": "ollama", "model": "m" }
  })");
  // Trailing blank lines are the file's shape, not part of what the model is told.
  ws.WritePersona("reviewer", "You review code and never write it.\n\n");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());
  const auto& reviewer = cfg.agents.at(1);

  CHECK(reviewer.backend.system_prompt.has_value());
  CHECK(*reviewer.backend.system_prompt == "You review code and never write it.");
}

TEST_CASE("A table naming both persona and system_prompt is refused", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reader", R"({
    "persona": "persona.md",
    "backend": { "gateway": "ollama", "model": "m", "system_prompt": "be brief" }
  })");
  ws.WritePersona("reader", "You read.\n");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("both persona and backend.system_prompt"));
}

TEST_CASE("A persona that is not there is refused by name", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reader", R"({
    "persona": "missing.md",
    "backend": { "gateway": "ollama", "model": "m" }
  })");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("persona file not found for reader"));
}

TEST_CASE("A directory whose table names another agent is refused", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reader", R"({
    "name": "writer",
    "backend": { "gateway": "ollama", "model": "m" }
  })");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("a directory is named after its agent"));
}

TEST_CASE("Two agents with one name are refused rather than merged", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(R"({
    "default_agent": "chat",
    "agents": [
      { "name": "chat", "backend": { "gateway": "ollama", "host": "http://x:1", "model": "m" } }
    ]
  })");
  ws.WriteAgent("chat", R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("two agents are named 'chat'"));
}

TEST_CASE("A directory without a table is not an agent", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  fs::create_directories(ws.AgentDir("notes"));

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());

  CHECK(cfg.agents.size() == 1);
}

TEST_CASE("A tools list naming something this build does not register is refused",
          "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("reader", R"({
    "tools": ["bash", "telepathy"],
    "backend": { "gateway": "ollama", "model": "m" }
  })");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("not a builtin tool"));
}


TEST_CASE("An MCP path in an agent's own table names something beside that table",
          "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("runner", R"({
    "backend": { "gateway": "ollama", "model": "m" },
    "mcp_servers": [
      { "name": "local", "command": "./serve.sh", "args": ["./serve.sh", "--verbose"] },
      { "name": "elsewhere", "command": "/usr/bin/other" }
    ]
  })");
  {
    std::ofstream script(ws.AgentDir("runner") / "serve.sh", std::ios::trunc);
    script << "#!/bin/sh\n";
  }

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());
  const auto& servers = cfg.agents.at(1).mcp_servers;
  REQUIRE(servers.size() == 2);

  // Named and present beside the table: rewritten so it runs from wherever the process is.
  CHECK(servers[0].command == (ws.AgentDir("runner") / "serve.sh").lexically_normal().string());
  // A flag is not a path, and must be handed to the server as written.
  CHECK(servers[0].args.at(1) == "--verbose");
  // An absolute command is already unambiguous, and is left alone.
  CHECK(servers[1].command == "/usr/bin/other");
}


TEST_CASE("An agent that delegates without ask_dir is a warning, not a refusal",
          "[agent_config][delegates]") {
  // The table is legal: `delegates` names who it may ask, `tools` names what it has, and the
  // pair is what a reader has to hold together to see that the delegation is unreachable.
  // `pu check` says so; loading does not refuse it, because a table that names a relation it
  // has no tool for is a mistake to point out rather than one to stop for.
  TempAgentWorkspace ws;
  ws.WriteTable(kInlineOnly);
  ws.WriteAgent("orchestrator", R"({
    "delegates": ["reader"],
    "tools": ["bash"],
    "backend": { "gateway": "ollama", "model": "m" }
  })");
  ws.WriteAgent("reader", R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());
  const auto orchestrator = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                                         [](const config::AgentEntry& entry) {
                                           return entry.name == "orchestrator";
                                         });
  REQUIRE(orchestrator != cfg.agents.end());
  CHECK(orchestrator->delegates == std::vector<std::string>{"reader"});
  CHECK(orchestrator->tools == std::vector<std::string>{"bash"});
}

TEST_CASE("default_agent may name an agent that lives in its own directory",
          "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(R"({ "default_agent": "reviewer", "agents": [] })");
  ws.WriteAgent("reviewer", R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(ws.Table().string());

  REQUIRE(cfg.agents.size() == 1);
  CHECK(cfg.agents[0].name == "reviewer");
}

TEST_CASE("A default_agent nothing configures lists what is configured", "[agent_config][delegates]") {
  TempAgentWorkspace ws;
  ws.WriteTable(R"({ "default_agent": "nobody", "agents": [] })");
  ws.WriteAgent("reviewer", R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  CHECK_THROWS_WITH(config::LoadAgentsConfig(ws.Table().string()),
                    Catch::Matchers::ContainsSubstring("have: reviewer"));
}

TEST_CASE("LoadAgentsConfig throws on missing file", "[agent_config]") {
  REQUIRE_THROWS_AS(config::LoadAgentsConfig("/nonexistent/path/agents.json"), std::runtime_error);
}

TEST_CASE("LoadAgentsConfig throws on invalid JSON", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  REQUIRE_THROWS_AS(config::LoadAgentsConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("LoadAgentsConfig works with explicit default_agent", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "only",
    "agents": [
      {
        "name": "only",
        "type": "llm",
        "backend": { "gateway": "ollama", "host": "http://localhost", "model": "x" }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.default_agent == "only");
}

TEST_CASE("A backend may leave out the host its gateway supplies", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" }
      },
      {
        "name": "cloud",
        "backend": { "gateway": "tencent", "model": "glm-5.3" }
      }
    ]
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());

  REQUIRE(cfg.agents[0].backend.host == "http://localhost:11434");
  REQUIRE(cfg.agents[1].backend.host == "https://copilot.tencent.com/v2");
}

TEST_CASE("A backend without a model is still refused", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      { "name": "chat", "backend": { "gateway": "ollama" } }
    ]
  })");

  REQUIRE_THROWS_AS(config::LoadAgentsConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("A backend naming no gateway is refused, and says which field is missing",
          "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      { "name": "chat", "backend": { "host": "http://localhost", "model": "x" } }
    ]
  })");

  try {
    (void)config::LoadAgentsConfig(tmp.path.string());
    FAIL("a backend with no gateway was accepted");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("Missing gateway") != std::string::npos);
    REQUIRE(std::string(e.what()).find("ollama") != std::string::npos);
  }
}

TEST_CASE("LoadAgentsConfig rejects an unknown default_agent", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "missing",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "host": "http://localhost", "model": "x" }
      }
    ]
  })");

  REQUIRE_THROWS_AS(config::LoadAgentsConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("Agents config writer produces valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "test";
  config::AgentEntry entry;
  entry.name = "test";
  entry.description = "desc";
  entry.backend.gateway = model::GatewayId::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), original));

  config::AgentsConfig loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.default_agent == "test");
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].name == "test");
  REQUIRE(loaded.agents[0].backend.gateway == model::GatewayId::kOpenAI);
  REQUIRE(loaded.agents[0].backend.host == "https://api.test.com");
  REQUIRE(loaded.agents[0].backend.api_key == "secret");
}

TEST_CASE("CreateBackend creates OllamaBackend", "[agent_config]") {
  model::Backend cfg;
  cfg.gateway = model::GatewayId::kOllama;
  cfg.host = "http://localhost:11434";
  cfg.model = "llama3.2";
  cfg.temperature = 0.5f;
  cfg.system_prompt = "Be helpful.";

  auto mock_http = std::make_shared<MockHttpClient>();
  auto backend = llm::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[agent_config]") {
  model::Backend cfg;
  cfg.gateway = model::GatewayId::kOpenAI;
  cfg.host = "https://api.openai.com/v1";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_shared<MockHttpClient>();
  auto backend = llm::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("ExpandEnvVars warns on undefined variable", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "x",
    "agents": [
      {
        "name": "x",
        "type": "llm",
        "backend": {
          "gateway": "ollama",
          "host": "http://localhost:11434",
          "model": "llama3.2",
          "system_prompt": "${UNDEFINED_VAR_FOR_TEST}"
        }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents[0].backend.system_prompt.has_value());
  REQUIRE(cfg.agents[0].backend.system_prompt->empty());
}

TEST_CASE("LoadAgentsConfig reads a thinking level", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "deepseek",
    "agents": [
      {
        "name": "deepseek",
        "backend": {
          "gateway": "openai",
          "host": "https://api.deepseek.com/v1",
          "model": "deepseek-reasoner",
          "thinking": "high"
        }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents[0].backend.thinking == ThinkingLevel::kHigh);
}

TEST_CASE("LoadAgentsConfig reads the context window a file names", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "cb",
    "agents": [
      {
        "name": "cb",
        "backend": {
          "gateway": "tencent",
          "model": "deepseek-v4.1-flash",
          "context_window": 250000
        }
      }
    ]
  })");
  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  // A nominal figure a product does not enforce is corrected here, so the file outranks the
  // catalogue: the number is kept as written rather than checked against anything.
  REQUIRE(cfg.agents[0].backend.context_window.has_value());
  REQUIRE(*cfg.agents[0].backend.context_window == 250000);
}

TEST_CASE("An absent context window stays absent", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "a",
    "agents": [{"name": "a", "backend": {"gateway": "ollama", "model": "m"}}]
  })");
  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  // Absent is not zero: absent is what sends the resolver to the gateway's own catalogue, and a
  // zero here would read as "this model holds nothing".
  REQUIRE_FALSE(cfg.agents[0].backend.context_window.has_value());
}

TEST_CASE("LoadAgentsConfig uses defaults for an absent backend option", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "host": "http://localhost:11434", "model": "qwen" }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents[0].backend.thinking == ThinkingLevel::kServerDefault);
}

TEST_CASE("Agents config writer round-trips a thinking level", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "deepseek";
  config::AgentEntry entry;
  entry.name = "deepseek";
  entry.backend.gateway = model::GatewayId::kOpenAI;
  entry.backend.host = "https://api.deepseek.com/v1";
  entry.backend.model = "deepseek-reasoner";
  entry.backend.thinking = ThinkingLevel::kMedium;
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), original));

  config::AgentsConfig loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].backend.thinking == ThinkingLevel::kMedium);
}

TEST_CASE("A rendered entry copies the file's own surroundings and expands nothing",
          "[agent_config]") {
  // The raw entry, as the file spells it: `api_key` is a `${VAR}`, not the secret.
  const boost::json::value source = json::parse(R"({
    "name": "source-agent",
    "description": "whatever it says",
    "backend": {
      "gateway": "tencent",
      "model": "some-model",
      "api_key": "${SOME_KEY}",
      "temperature": 0.3,
      "max_tokens": 4096,
      "thinking": "high"
    },
    "security": {"sandbox_root": ".", "forbidden_patterns": ["the-change-directory-word"]},
    "tool_call_budget": 100
  })");

  model::ModelEntry model{"glm-5.3-flash", {}};
  model.caps.credits = "x0.06 credits";

  const std::string rendered = config::FormatAgentEntry(model, source);

  REQUIRE(rendered.find("\"name\": \"tencent-glm-5.3-flash\"") != std::string::npos);
  REQUIRE(rendered.find("\"model\": \"glm-5.3-flash\"") != std::string::npos);
  // Copied, not resolved: a snippet goes into a file that may be committed.
  REQUIRE(rendered.find("${SOME_KEY}") != std::string::npos);
  REQUIRE(rendered.find("\"max_tokens\": 4096") != std::string::npos);
  REQUIRE(rendered.find("\"tool_call_budget\": 100") != std::string::npos);
  REQUIRE(rendered.find("x0.06 credits") != std::string::npos);
  // A double is written plainly, not in the exponent form boost::json picks.
  REQUIRE(rendered.find("\"temperature\": 0.3") != std::string::npos);
  REQUIRE(rendered.find("3E-1") == std::string::npos);
}

TEST_CASE("A rendered entry names a level a reasoning-only model cannot run without",
          "[agent_config]") {
  const boost::json::value source = json::parse(R"({
    "name": "source-agent",
    "backend": {"gateway": "tencent", "model": "some-model", "api_key": "${K}"}
  })");

  model::ModelEntry model{"glm-5.1", {}};
  model.caps.only_reasoning = true;
  model.caps.default_effort = "medium";

  const std::string rendered = config::FormatAgentEntry(model, source);
  REQUIRE(rendered.find("\"thinking\": \"medium\"") != std::string::npos);
}

TEST_CASE("A rendered entry leaves a model that does not require a level alone",
          "[agent_config]") {
  const boost::json::value source = json::parse(R"({
    "name": "source-agent",
    "backend": {"gateway": "openai", "host": "https://api.deepseek.com/v1", "model": "m"}
  })");

  const std::string rendered = config::FormatAgentEntry(model::ModelEntry{"plain", {}}, source);
  REQUIRE(rendered.find("thinking") == std::string::npos);
  // The vendor's own gateway leaves the model id as the name.
  REQUIRE(rendered.find("\"name\": \"plain\"") != std::string::npos);
}

TEST_CASE("FindRawAgentEntry reads the file as written, not as resolved", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "a",
    "agents": [
      {"name": "a", "backend": {"gateway": "tencent", "model": "m1", "api_key": "${KEY_A}"}},
      {"name": "b", "backend": {"gateway": "openai", "model": "m2"}}
    ]
  })");

  const std::optional<boost::json::value> found =
      config::FindRawAgentEntry(tmp.path.string(), "a");
  REQUIRE(found.has_value());
  REQUIRE(boost::json::serialize(*found).find("${KEY_A}") != std::string::npos);

  REQUIRE_FALSE(config::FindRawAgentEntry(tmp.path.string(), "absent").has_value());
}

// What a session will spend on keeping a command's output. The block is per agent, so
// a small local model and a 1M-context cloud model can want different page sizes.
TEST_CASE("A paging block sets the budget a session runs with", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "paging": { "page_lines": 40, "max_lines": 90, "capture_bytes": 4096,
                    "retain_bytes": 8192, "retain_buffers": 3 }
      }
    ]
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  const paging::Budget& budget = cfg.agents[0].paging;

  REQUIRE(budget.page_lines == 40);
  REQUIRE(budget.max_lines == 90);
  REQUIRE(budget.capture_bytes == 4096);
  REQUIRE(budget.retain_bytes == 8192);
  REQUIRE(budget.retain_buffers == 3);
}

// A page is a page: a `page_lines` above `max_lines` would be silently narrowed, and a
// store that retains nothing cannot be paged at all. Both are raised, not refused, so a
// typo in one number does not keep the agent from starting.
TEST_CASE("A paging block that cannot be obeyed is raised to one that can", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "paging": { "page_lines": 0, "max_lines": 0, "capture_bytes": 1,
                    "retain_bytes": 1, "retain_buffers": 0 }
      }
    ]
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  const paging::Budget& budget = cfg.agents[0].paging;

  REQUIRE(budget.page_lines == 1);
  REQUIRE(budget.max_lines == 1);  // raised to page_lines, which was itself raised to 1
  REQUIRE(budget.capture_bytes == 1024);
  REQUIRE(budget.retain_bytes == 1024);
  REQUIRE(budget.retain_buffers == 1);
}

TEST_CASE("An agent with no paging block keeps the default budget", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      { "name": "chat", "backend": { "gateway": "ollama", "model": "qwen" } }
    ]
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  const paging::Budget defaults;

  REQUIRE(cfg.agents[0].paging.page_lines == defaults.page_lines);
  REQUIRE(cfg.agents[0].paging.max_lines == defaults.max_lines);
  REQUIRE(cfg.agents[0].paging.capture_bytes == defaults.capture_bytes);
  REQUIRE(cfg.agents[0].paging.retain_bytes == defaults.retain_bytes);
  REQUIRE(cfg.agents[0].paging.retain_buffers == defaults.retain_buffers);
}

// `output` was the block's name before it grew retention as well as page size. A config
// written then must still load: the old key is ignored, not fatal.
TEST_CASE("An output block from an older config is ignored, not fatal", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "output": { "page_lines": 40 }
      }
    ]
  })");

  const config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());

  REQUIRE(cfg.agents[0].paging.page_lines == paging::Budget{}.page_lines);
}
