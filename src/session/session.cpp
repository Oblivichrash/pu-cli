// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/session.hpp"
#include "pu/http_client.hpp"
#include "infra/curl_http_client.hpp"
#include "pu/error.hpp"
#include "pu/llm/provider_factory.hpp"

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
  // Delegate to the shared factory so provider construction has one source of truth.
  auto http = std::make_unique<pu::http::CurlHttpClient>();
  return config::CreateBackend(runtime_spec_.backend, std::move(http));
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
