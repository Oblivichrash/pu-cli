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
}

std::unique_ptr<LLMProvider> Session::CreateProvider() const {
  return config::CreateBackend(runtime_spec_.backend,
                               std::make_unique<pu::http::BeastHttpClient>());
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
