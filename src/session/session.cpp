// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/session.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"
#include "pu/http_client.hpp"
#include "infra/curl_http_client.hpp"
#include "pu/error.hpp"

namespace pu {

Session::Session(const std::string& id, const std::string& owner_id)
  : id_(id), owner_id_(owner_id),
    created_at_(std::chrono::system_clock::now()),
    last_access_at_(created_at_),
    workspace_(std::make_shared<Workspace>()),
    runtime_spec_() {}

Session::Session(const std::string& id, const std::string& owner_id,
                 std::shared_ptr<Workspace> workspace,
                 const RuntimeSpec& spec)
  : id_(id), owner_id_(owner_id),
    created_at_(std::chrono::system_clock::now()),
    last_access_at_(created_at_),
    workspace_(std::move(workspace)),
    runtime_spec_(spec) {}

void Session::SwitchAgent(const std::string& agent_name) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
      "Cannot switch agent while tool calls are pending. "
      "Please let the current tool finish or /clear.");
  }
  runtime_spec_.agent_name = agent_name;
}

void Session::SwitchBackend(const config::BackendConfig& new_config) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
      "Cannot switch backend while tool calls are pending. "
      "Please let the current tool finish or /clear.");
  }
  runtime_spec_.backend = new_config;
  // Include the configured system prompt in the static system message.
  workspace_->SetVar("system_prompt", new_config.system_prompt.value_or(""));
}

std::unique_ptr<LLMProvider> Session::CreateProvider() const {
  const auto& cfg = runtime_spec_.backend;
  auto http = std::make_unique<pu::http::CurlHttpClient>();
  if (cfg.type == config::BackendType::kOllama) {
    OllamaProvider::Config ollama_cfg;
    ollama_cfg.model = cfg.model;
    ollama_cfg.temperature = cfg.temperature;
    ollama_cfg.host = cfg.host;
    ollama_cfg.api_key = cfg.api_key.value_or("");
    ollama_cfg.max_tokens = cfg.max_tokens;
    return std::make_unique<OllamaProvider>(std::move(ollama_cfg), std::move(http));
  } else if (cfg.type == config::BackendType::kOpenAI) {
    OpenAIProvider::Config openai_cfg;
    openai_cfg.model = cfg.model;
    openai_cfg.temperature = cfg.temperature;
    openai_cfg.host = cfg.host;
    openai_cfg.api_key = cfg.api_key.value_or("");
    openai_cfg.parameters_as_string = cfg.parameters_as_string;
    openai_cfg.max_tokens = cfg.max_tokens;
    openai_cfg.enable_thinking = cfg.enable_thinking;
    return std::make_unique<OpenAIProvider>(openai_cfg, std::move(http));
  }
  throw RuntimeError("Unknown backend type");
}

nlohmann::json Session::Serialize() const {
  nlohmann::json j;
  j["id"] = id_;
  j["owner_id"] = owner_id_;
  j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
      created_at_.time_since_epoch()).count();
  j["last_access_at"] = std::chrono::duration_cast<std::chrono::seconds>(
      last_access_at_.time_since_epoch()).count();
  j["workspace"] = workspace_->Serialize();
  j["runtime_spec"] = runtime_spec_.Serialize();
  return j;
}

std::unique_ptr<Session> Session::Deserialize(const nlohmann::json& j) {
  auto id = j["id"].get<std::string>();
  auto owner_id = j["owner_id"].get<std::string>();
  auto ws = Workspace::Deserialize(j["workspace"]);
  auto spec = RuntimeSpec::Deserialize(j["runtime_spec"]);
  auto session = std::make_unique<Session>(id, owner_id, ws, spec);
  if (j.contains("created_at")) {
    auto secs = std::chrono::seconds(j["created_at"].get<int64_t>());
    session->created_at_ = std::chrono::system_clock::time_point(secs);
  }
  if (j.contains("last_access_at")) {
    auto secs = std::chrono::seconds(j["last_access_at"].get<int64_t>());
    session->last_access_at_ = std::chrono::system_clock::time_point(secs);
  }
  return session;
}

} // namespace pu
