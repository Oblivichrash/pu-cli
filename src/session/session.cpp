// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/session.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"
#include "pu/http_client.hpp"
#include "infra/curl_http_client.hpp"
#include "pu/error.hpp"

#include <boost/json.hpp>

namespace pu {

Session::Session()
  : workspace_(std::make_shared<Workspace>()),
    runtime_spec_() {}

Session::Session(std::shared_ptr<Workspace> workspace)
  : workspace_(std::move(workspace)),
    runtime_spec_() {}

Session::Session(std::shared_ptr<Workspace> workspace, const RuntimeSpec& spec)
  : workspace_(std::move(workspace)),
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
  workspace_->SetVar("system_prompt", boost::json::value(new_config.system_prompt.value_or("")));
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

boost::json::value Session::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["workspace"] = workspace_->Serialize();
  j.as_object()["runtime_spec"] = runtime_spec_.Serialize();
  return j;
}

std::unique_ptr<Session> Session::Deserialize(const boost::json::value& j) {
  auto ws = Workspace::Deserialize(j.at("workspace"));
  auto spec = RuntimeSpec::Deserialize(j.at("runtime_spec"));
  auto session = std::make_unique<Session>(ws, spec);
  return session;
}

} // namespace pu
