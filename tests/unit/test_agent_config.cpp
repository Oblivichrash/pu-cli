// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/agents.hpp"
#include "pu/config/setup.hpp"
#include "pu/model/backend.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/logging.hpp"
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

void WriteAgentsConfigForTest(const std::string& config_path, const config::AgentEntry& entry) {
  json::value j = {
      {"name", entry.name},
      {"description", entry.description},
  };
  json::value security = {
      {"sandbox_root", entry.security.sandbox_root},
      {"max_command_length", entry.security.max_command_length},
  };
  security.as_object()["forbidden_patterns"] =
      boost::json::value_from(entry.security.forbidden_patterns);
  j.as_object()["security"] = security;

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
  j.as_object()["backend"] = backend;

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
    j.as_object()["mcp_servers"] = mcp_array;
  }

  std::ofstream file(config_path);
  if (!file.is_open()) throw pu::GenericError("Failed to open config file for writing: " + config_path);
  file << json::PrettyPrint(j);
}

}  // namespace

TEST_CASE("FindConfigPath prefers the workspace's .pu/agents.json", "[agent_config]") {
  auto dir = UniqueTempPath("pu_findconfig_project");
  auto home = UniqueTempPath("pu_findconfig_home");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir / ".pu");
  fs::create_directories(home);

  ScopedWorkspaceRoot root(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    std::ofstream f(dir / ".pu" / "agents.json");
    f << "{}";

    // The answer is the file this run is about, given as a path that can be opened from
    // anywhere: a relative "./.pu/agents.json" would mean a different file to a caller that
    // had since changed directory.
    REQUIRE(config::FindConfigPath() == (dir / ".pu" / "agents.json").string());
  }

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

  ScopedWorkspaceRoot root(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    std::ofstream f(home / ".pu" / "agents.json");
    f << "{}";

    REQUIRE(config::FindConfigPath() == (home / ".pu" / "agents.json").string());
  }

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

  ScopedWorkspaceRoot root(dir);

  {
    ScopedEnvVar env("HOME", home.string());
    REQUIRE(config::FindConfigPath().empty());
  }

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

  // The options are read from the run's own table, which is the directory named here and not
  // whichever one this process was started in.
  ScopedWorkspaceRoot root(dir);

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

  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

// A file that describes one agent has nothing to choose between and nothing to default to:
// the name is at the top and the backend beneath it, which is the whole of what a workspace
// with one agent has to record.
TEST_CASE("A file describing one agent needs no default_agent and no agents array",
          "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "name": "solo",
    "backend": { "gateway": "ollama", "model": "qwen2.5:0.5b" }
  })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.name == "solo");
  REQUIRE(cfg.backend.gateway == model::GatewayId::kOllama);
  REQUIRE(cfg.backend.model == "qwen2.5:0.5b");
  // The host was left out, so it is the gateway's own rather than empty: a backend with no
  // address named the one the gateway always has.
  REQUIRE(cfg.backend.host == model::DefaultHostFor(model::GatewayId::kOllama));
}

TEST_CASE("A single-agent file that does not name itself is refused", "[agent_config][single]") {
  TempConfigFile tmp;
  tmp.write(R"({ "backend": { "gateway": "ollama", "model": "m" } })");

  // There is no directory and no table to supply the name, and a name is what a session
  // records, so nobody but the file's author can answer it.
  REQUIRE_THROWS_WITH(config::LoadAgentConfig(tmp.path.string()),
                      Catch::Matchers::ContainsSubstring("must name itself"));
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
    const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
    REQUIRE(cfg.backend.api_key.has_value());
    REQUIRE(*cfg.backend.api_key == "sk-from-the-environment");
  }

  SECTION("unset") {
    // Nothing to take, so nothing is invented: the backend has no key and the first turn
    // says so, rather than carrying an empty one that reads as a key which is wrong.
    ScopedEnvVar env("TENCENT_API_KEY", "");
    const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
    REQUIRE(!cfg.backend.api_key.has_value());
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
  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.backend.api_key.has_value());
  REQUIRE(*cfg.backend.api_key == "from-the-file");
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
  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  // A key that expanded to nothing keeps the empty answer the author wrote rather than the
  // gateway's variable: the variable they named is the one they meant, and taking another one
  // would hide the typo. `pu check` reports this as "(empty)" with a warning.
  REQUIRE(cfg.backend.api_key.has_value());
  REQUIRE(cfg.backend.api_key->empty());
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
  const config::AgentEntry kept =
      config::LoadAgentConfig((workspace / ".pu" / "agents.json").string());
  REQUIRE(kept.backend.model == "m");

  fs::remove_all(workspace, ec);
}

TEST_CASE("LoadAgentsConfig parses valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
        "name": "chat",
        "description": "Local Chat",
        "backend": {
          "gateway": "ollama",
          "host": "http://localhost:11434",
          "model": "qwen3.5:4b",
          "temperature": 0.7
        },
        "security": {
          "sandbox_root": "/tmp"
        }
      })";
  tmp.write(json);

  {
    config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());

    REQUIRE(cfg.name == "chat");
    REQUIRE(cfg.backend.gateway == model::GatewayId::kOllama);
    REQUIRE(cfg.backend.model == "qwen3.5:4b");
    REQUIRE(cfg.security.sandbox_root == "/tmp");
  }
}

















TEST_CASE("LoadAgentsConfig throws on missing file", "[agent_config]") {
  REQUIRE_THROWS_AS(config::LoadAgentConfig("/nonexistent/path/agents.json"), std::runtime_error);
}

TEST_CASE("LoadAgentsConfig throws on invalid JSON", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  REQUIRE_THROWS_AS(config::LoadAgentConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("LoadAgentConfig reads a one-agent file", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
        "name": "only",
        "backend": { "gateway": "ollama", "host": "http://localhost", "model": "x" }
      })";
  tmp.write(json);
  config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.name == "only");
}

TEST_CASE("A backend may leave out the host its gateway supplies", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" }
      })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());

  REQUIRE(cfg.backend.host == "http://localhost:11434");
}

TEST_CASE("A backend without a model is still refused", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({ "name": "chat", "backend": { "gateway": "ollama" } })");

  REQUIRE_THROWS_AS(config::LoadAgentConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("A backend naming no gateway is refused, and says which field is missing",
          "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({ "name": "chat", "backend": { "host": "http://localhost", "model": "x" } })");

  try {
    (void)config::LoadAgentConfig(tmp.path.string());
    FAIL("a backend with no gateway was accepted");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("Missing gateway") != std::string::npos);
    REQUIRE(std::string(e.what()).find("ollama") != std::string::npos);
  }
}

TEST_CASE("A file without a name is refused", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
        "backend": { "gateway": "ollama", "host": "http://localhost", "model": "x" }
      })");

  REQUIRE_THROWS_AS(config::LoadAgentConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("Agents config writer produces valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentEntry entry;
  entry.name = "test";
  entry.description = "desc";
  entry.backend.gateway = model::GatewayId::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), entry));

  config::AgentEntry loaded = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(loaded.name == "test");
  REQUIRE(loaded.backend.gateway == model::GatewayId::kOpenAI);
  REQUIRE(loaded.backend.host == "https://api.test.com");
  REQUIRE(loaded.backend.api_key == "secret");
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
        "name": "x",
        "backend": {
          "gateway": "ollama",
          "host": "http://localhost:11434",
          "model": "llama3.2",
          "system_prompt": "${UNDEFINED_VAR_FOR_TEST}"
        }
      })";
  tmp.write(json);
  config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.backend.system_prompt.has_value());
  REQUIRE(cfg.backend.system_prompt->empty());
}

TEST_CASE("LoadAgentsConfig reads a thinking level", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
        "name": "deepseek",
        "backend": {
          "gateway": "openai",
          "host": "https://api.deepseek.com/v1",
          "model": "deepseek-reasoner",
          "thinking": "high"
        }
      })";
  tmp.write(json);
  config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.backend.thinking == ThinkingLevel::kHigh);
}

TEST_CASE("LoadAgentsConfig reads the context window a file names", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
        "name": "cb",
        "backend": {
          "gateway": "tencent",
          "model": "deepseek-v4.1-flash",
          "context_window": 250000
        }
      })");
  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  // A nominal figure a product does not enforce is corrected here, so the file outranks the
  // catalogue: the number is kept as written rather than checked against anything.
  REQUIRE(cfg.backend.context_window.has_value());
  REQUIRE(*cfg.backend.context_window == 250000);
}

TEST_CASE("An absent context window stays absent", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({"name": "a", "backend": {"gateway": "ollama", "model": "m"} })");
  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  // Absent is not zero: absent is what sends the resolver to the gateway's own catalogue, and a
  // zero here would read as "this model holds nothing".
  REQUIRE_FALSE(cfg.backend.context_window.has_value());
}

TEST_CASE("LoadAgentsConfig uses defaults for an absent backend option", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
        "name": "chat",
        "backend": { "gateway": "ollama", "host": "http://localhost:11434", "model": "qwen" }
      })";
  tmp.write(json);
  config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(cfg.backend.thinking == ThinkingLevel::kServerDefault);
}

TEST_CASE("Agents config writer round-trips a thinking level", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentEntry entry;
  entry.name = "deepseek";
  entry.backend.gateway = model::GatewayId::kOpenAI;
  entry.backend.host = "https://api.deepseek.com/v1";
  entry.backend.model = "deepseek-reasoner";
  entry.backend.thinking = ThinkingLevel::kMedium;

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), entry));

  config::AgentEntry loaded = config::LoadAgentConfig(tmp.path.string());
  REQUIRE(loaded.backend.thinking == ThinkingLevel::kMedium);
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
    "security": {"sandbox_root": ".", "forbidden_patterns": ["the-change-directory-word"]}
  })");

  model::ModelEntry model{"glm-5.3-flash", {}};
  model.caps.credits = "x0.06 credits";

  const std::string rendered = config::FormatAgentEntry(model, source);

  REQUIRE(rendered.find("\"name\": \"tencent-glm-5.3-flash\"") != std::string::npos);
  REQUIRE(rendered.find("\"model\": \"glm-5.3-flash\"") != std::string::npos);
  // Copied, not resolved: a snippet goes into a file that may be committed.
  REQUIRE(rendered.find("${SOME_KEY}") != std::string::npos);
  REQUIRE(rendered.find("\"max_tokens\": 4096") != std::string::npos);
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
  tmp.write(R"({"name": "a", "backend": {"gateway": "tencent", "model": "m1", "api_key": "${KEY_A}"} })");

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
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "paging": { "page_lines": 40, "max_lines": 90, "capture_bytes": 4096,
                    "retain_bytes": 8192, "retain_buffers": 3 }
      })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  const paging::Budget& budget = cfg.paging;

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
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "paging": { "page_lines": 0, "max_lines": 0, "capture_bytes": 1,
                    "retain_bytes": 1, "retain_buffers": 0 }
      })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  const paging::Budget& budget = cfg.paging;

  REQUIRE(budget.page_lines == 1);
  REQUIRE(budget.max_lines == 1);  // raised to page_lines, which was itself raised to 1
  REQUIRE(budget.capture_bytes == 1024);
  REQUIRE(budget.retain_bytes == 1024);
  REQUIRE(budget.retain_buffers == 1);
}

TEST_CASE("An agent with no paging block keeps the default budget", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({ "name": "chat", "backend": { "gateway": "ollama", "model": "qwen" } })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  const paging::Budget defaults;

  REQUIRE(cfg.paging.page_lines == defaults.page_lines);
  REQUIRE(cfg.paging.max_lines == defaults.max_lines);
  REQUIRE(cfg.paging.capture_bytes == defaults.capture_bytes);
  REQUIRE(cfg.paging.retain_bytes == defaults.retain_bytes);
  REQUIRE(cfg.paging.retain_buffers == defaults.retain_buffers);
}

// `output` was the block's name before it grew retention as well as page size. A config
// written then must still load: the old key is ignored, not fatal.
TEST_CASE("An output block from an older config is ignored, not fatal", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write(R"({
        "name": "chat",
        "backend": { "gateway": "ollama", "model": "qwen" },
        "output": { "page_lines": 40 }
      })");

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());

  REQUIRE(cfg.paging.page_lines == paging::Budget{}.page_lines);
}

TEST_CASE("A backend write keeps the file's own spelling of every key it was not asked to change",
          "[agent_config][write]") {
  TempConfigFile tmp;
  tmp.write(R"({
    "name": "a",
    "description": "kept",
    "backend": {
      "gateway": "tencent",
      "model": "m",
      "api_key": "$TENCENT_API_KEY",
      "temperature": 0.3
    },
    "delegates": ["b"]
  })");

  config::BackendUpdate update;
  update.gateway = "ollama";
  update.model = "qwen";
  config::UpdateAgentBackend(tmp.path.string(), update);

  std::ifstream in(tmp.path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(text.find("$TENCENT_API_KEY") != std::string::npos);
  CHECK(text.find("\"description\"") != std::string::npos);
  CHECK(text.find("\"delegates\"") != std::string::npos);

  const config::AgentEntry cfg = config::LoadAgentConfig(tmp.path.string());
  CHECK(cfg.backend.gateway == model::GatewayId::kOllama);
  CHECK(cfg.backend.model == "qwen");
  CHECK(cfg.backend.host == model::DefaultHostFor(model::GatewayId::kOllama));
}

TEST_CASE("A backend write leaves no staged file behind", "[agent_config][write]") {
  TempConfigFile tmp;
  tmp.write(R"({"name": "a", "backend": {"gateway": "ollama", "model": "m"}})");

  config::BackendUpdate update;
  update.gateway = "ollama";
  update.model = "qwen";
  config::UpdateAgentBackend(tmp.path.string(), update);

  std::size_t leftovers = 0;
  for (const auto& entry : fs::directory_iterator(fs::path(tmp.path).parent_path())) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with(fs::path(tmp.path).filename().string()) && name.ends_with(".tmp")) ++leftovers;
  }
  CHECK(leftovers == 0);
}
