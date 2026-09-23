// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/session.hpp"
#include "pu/infra/http_client.hpp"
#include "pu/infra/beast_http_client.hpp"
#include "pu/core/error.hpp"

#include <boost/json.hpp>

namespace pu {

Session::Session()
  : workspace_(std::make_shared<Workspace>()),
    runtime_spec_() {}

Session::Session(std::shared_ptr<Workspace> workspace, const RuntimeSpec& spec)
  : workspace_(std::move(workspace)),
    runtime_spec_(spec) {}

void Session::SetAgent(const std::string& agent_name) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
      "Cannot switch agent while tool calls are pending. "
      "Please let the current tool finish or /clear.");
  }
  // Choosing an agent drops the override, so the agent's own configuration
  // becomes the source of the backend again.
  runtime_spec_.agent_name = agent_name;
  runtime_spec_.backend_override.reset();
}

void Session::SetBackendOverride(const config::BackendConfig& new_config) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
      "Cannot switch backend while tool calls are pending. "
      "Please let the current tool finish or /clear.");
  }
  runtime_spec_.backend_override = new_config;
}

std::unique_ptr<LLMProvider> Session::CreateProvider(
    const config::BackendConfig& backend) const {
  return config::CreateBackend(backend,
                               std::make_unique<pu::http::BeastHttpClient>());
}

boost::json::value Session::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["schema_version"] = context::kSchemaVersion;
  j.as_object()["workspace"] = workspace_->Serialize();
  j.as_object()["runtime_spec"] = runtime_spec_.Serialize();
  return j;
}

std::unique_ptr<Session> Session::Deserialize(const boost::json::value& j) {
  const bool has_version = json::HasKey(j, "schema_version");
  const int version = json::ValueOrDefault<int>(j, "schema_version", 0);
  if (!has_version || version != context::kSchemaVersion) return nullptr;

  // A version field alone is not enough: an unreachable branch used the same
  // number for a different layout, so the storage itself has to look like a DAG.
  if (!json::HasKey(j, "workspace") ||
      !json::HasKey(j.at("workspace"), "history") ||
      !j.at("workspace").at("history").is_object()) {
    return nullptr;
  }

  auto ws = Workspace::Deserialize(j.at("workspace"));
  if (!ws) return nullptr;

  // The runtime section is what selects the agent, so a file without it would
  // load as a conversation that cannot reach a model.
  if (!json::HasKey(j, "runtime_spec") || !j.at("runtime_spec").is_object()) {
    return nullptr;
  }
  std::optional<RuntimeSpec> spec = RuntimeSpec::Deserialize(j.at("runtime_spec"));
  if (!spec) return nullptr;

  return std::make_unique<Session>(ws, *spec);
}

} // namespace pu
