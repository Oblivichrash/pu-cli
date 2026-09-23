// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/session.hpp"

#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/core/text.hpp"
#include "pu/session/request.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace pu {

namespace {

std::vector<context::ContentPart> ToContent(const std::string& text) {
  std::vector<context::ContentPart> content;
  content.emplace_back(context::TextPart{text});
  return content;
}

context::MessagePayload ToPayload(const ChatMessage& msg) {
  std::vector<context::ContentPart> content = ToContent(msg.content);

  if (msg.role == context::kAssistantRole) {
    context::AssistantPayload assistant;
    assistant.content = std::move(content);
    if (!msg.reasoning_content.empty()) {
      // The legacy field is the reasoning text as the provider sent it.
      assistant.reasoning = context::Reasoning{"", "", msg.reasoning_content};
    }
    if (msg.tool_calls.is_array()) {
      for (const boost::json::value& call : msg.tool_calls.as_array()) {
        assistant.tool_calls.push_back(context::ToolCallFromJson(call));
      }
    }
    return assistant;
  }

  if (msg.role == context::kToolRole || msg.role == "tool_result") {
    context::ToolPayload receipt;
    receipt.tool_call_id = msg.tool_call_id;
    receipt.tool_name = msg.tool_name;
    receipt.content = std::move(content);
    return receipt;
  }

  if (msg.role == context::kSystemRole) {
    context::SystemPayload system;
    system.content = std::move(content);
    return system;
  }

  context::UserPayload user;
  user.content = std::move(content);
  return user;
}

// Storage holds text that arrived from outside pu-cli, which may be in the
// producer's encoding: a localized library message, for instance, is written in
// the system locale. It is normalised here, the one way into storage, so that
// neither the request view nor the session file can inherit invalid bytes.
ChatMessage Normalized(const ChatMessage& msg) {
  if (text::IsValidUtf8(msg.content) && text::IsValidUtf8(msg.tool_name) &&
      text::IsValidUtf8(msg.reasoning_content) && text::IsValidUtf8(msg.tool_call_id)) {
    return msg;
  }
  ChatMessage clean = msg;
  clean.content = text::SanitizeUtf8(msg.content);
  clean.tool_name = text::SanitizeUtf8(msg.tool_name);
  clean.reasoning_content = text::SanitizeUtf8(msg.reasoning_content);
  clean.tool_call_id = text::SanitizeUtf8(msg.tool_call_id);
  return clean;
}

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

}  // namespace

void Transcript::Append(const ChatMessage& msg) {
  graph_.AppendAfterLeaf(ToPayload(Normalized(msg)), msg.timestamp);
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  std::vector<ChatMessage> history;
  for (const context::MessageNode* node : graph_.Chain()) {
    history.push_back(session::RenderMessage(*node, static_cast<int>(history.size()) + 1));
  }
  return history;
}

size_t Transcript::Size() const { return graph_.Size(); }

bool Transcript::HasPendingToolCalls() const {
  return graph_.LeafHasUnfinishedToolCalls();
}

boost::json::value Transcript::Serialize() const { return graph_.Serialize(); }

bool Transcript::Deserialize(const boost::json::value& j, Transcript& out) {
  context::MessageGraph graph;
  if (!context::MessageGraph::Deserialize(j, graph)) return false;
  out.graph_ = std::move(graph);
  return true;
}

void Workspace::Append(const ChatMessage& msg) {
  transcript_.Append(msg);
}

void Workspace::Append(const std::string& role, const std::string& content) {
  ChatMessage msg;
  msg.timestamp = CurrentTimestamp();
  msg.role = role;
  msg.content = content;
  Append(msg);
}

std::vector<ChatMessage> Workspace::GetHistory() const {
  return transcript_.GetHistory();
}

size_t Workspace::HistorySize() const {
  return transcript_.Size();
}

bool Workspace::HasPendingToolCalls() const {
  return transcript_.HasPendingToolCalls();
}

boost::json::value Workspace::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["history"] = transcript_.Serialize();
  return j;
}

std::shared_ptr<Workspace> Workspace::Deserialize(const boost::json::value& j) {
  auto ws = std::make_shared<Workspace>();

  if (json::HasKey(j, "history")) {
    if (!Transcript::Deserialize(j.at("history"), ws->transcript_)) return nullptr;
  }

  return ws;
}

void Workspace::ClearHistory() {
  transcript_ = Transcript{};
}

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

}  // namespace pu
