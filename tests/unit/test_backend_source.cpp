// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "pu/agent_config.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

namespace fs = std::filesystem;
using namespace pu;

namespace {

// A workspace the runtime can start from, with one agent whose backend the test
// rewrites between starts.
class BackendSourceFixture {
public:
  BackendSourceFixture() {
    static int counter = 0;
    root_ = fs::temp_directory_path() / ("pu_backend_source_" + std::to_string(counter++));
    fs::create_directories(root_ / ".pu");
    Write(0.3f);
  }

  ~BackendSourceFixture() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  const fs::path& root() const { return root_; }

  void Write(float temperature) {
    boost::json::value cfg = {
      {"default_agent", "chat"},
      {"agents",
       boost::json::array{boost::json::value{
           {"name", "chat"},
           {"backend",
            {
                {"type", "openai"},
                {"host", "http://127.0.0.1:1"},
                {"model", "test-model"},
                {"temperature", temperature},
                {"enable_thinking", false},
            }},
       }}},
    };
    std::ofstream out(root_ / ".pu" / "agents.json", std::ios::trunc);
    out << boost::json::serialize(cfg);
  }

private:
  fs::path root_;
};

}  // namespace

TEST_CASE("A backend keeps enable_thinking through a round trip", "[backend]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kOpenAI;
  cfg.host = "http://127.0.0.1:1";
  cfg.model = "test-model";
  cfg.enable_thinking = false;

  const config::BackendConfig read =
      boost::json::value_to<config::BackendConfig>(boost::json::value_from(cfg));

  REQUIRE(read.enable_thinking == false);
  REQUIRE(read.model == "test-model");
}

TEST_CASE("A session spec carries a backend only when one was overridden", "[backend]") {
  RuntimeSpec chosen;
  chosen.agent_name = "chat";

  const boost::json::value written = chosen.Serialize();
  REQUIRE(written.as_object().count("agent_name") == 1);
  REQUIRE(written.as_object().count("backend_override") == 0);

  config::BackendConfig override_cfg;
  override_cfg.model = "overridden";
  override_cfg.enable_thinking = false;
  chosen.backend_override = override_cfg;

  const auto restored = RuntimeSpec::Deserialize(chosen.Serialize());
  REQUIRE(restored.has_value());
  REQUIRE(restored->agent_name == "chat");
  REQUIRE(restored->backend_override.has_value());
  REQUIRE(restored->backend_override->model == "overridden");
  REQUIRE(restored->backend_override->enable_thinking == false);
}

TEST_CASE("Editing agents.json is enough to change the backend", "[backend]") {
  BackendSourceFixture fixture;

  {
    Runtime runtime;
    REQUIRE(runtime.SwitchWorkspace(fixture.root()));
    REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.3f));
  }

  fixture.Write(0.9f);

  {
    Runtime runtime;
    REQUIRE(runtime.SwitchWorkspace(fixture.root()));
    REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.9f));
  }
}

TEST_CASE("A session override wins over the configured backend", "[backend]") {
  BackendSourceFixture fixture;
  Runtime runtime;
  REQUIRE(runtime.SwitchWorkspace(fixture.root()));

  auto session = runtime.GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.3f));

  config::BackendConfig override_cfg = runtime.CurrentBackend();
  override_cfg.temperature = 1.5f;
  session->SetBackendOverride(override_cfg);

  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(1.5f));
}
