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

context::MessagePayload ToPayload(const ChatMessage& msg) {
  if (msg.role == context::kAssistantRole) {
    context::AssistantPayload assistant;
    assistant.content = msg.content;
    if (!msg.reasoning_content.empty()) {
      // The legacy field is the reasoning text as the provider sent it.
      assistant.reasoning = context::Reasoning{msg.reasoning_content};
    }
    if (msg.tool_calls.is_array()) {
      for (const boost::json::value& call : msg.tool_calls.as_array()) {
        assistant.tool_calls.push_back(context::ToolCallFromJson(call));
      }
    }
    return assistant;
  }

  if (msg.role == context::kToolRole) {
    context::ToolPayload receipt;
    receipt.tool_call_id = msg.tool_call_id;
    receipt.tool_name = msg.tool_name;
    receipt.content = msg.content;
    return receipt;
  }

  if (msg.role == context::kSystemRole) {
    context::SystemPayload system;
    system.content = msg.content;
    return system;
  }

  context::UserPayload user;
  user.content = msg.content;
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

void Conversation::Append(const ChatMessage& msg) {
  graph_.AppendAfterLeaf(ToPayload(Normalized(msg)), msg.timestamp);
}

void Conversation::Append(const std::string& role, const std::string& content) {
  ChatMessage msg;
  msg.timestamp = CurrentTimestamp();
  msg.role = role;
  msg.content = content;
  Append(msg);
}

std::vector<ChatMessage> Conversation::GetHistory() const {
  std::vector<ChatMessage> history;
  for (const context::MessageNode* node : graph_.Chain()) {
    history.push_back(session::RenderMessage(*node, static_cast<int>(history.size()) + 1));
  }
  return history;
}

bool Conversation::RewindBefore(size_t turn) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
        "Cannot rewind while tool calls are pending. "
        "Please let the current tool finish or /clear.");
  }
  const std::vector<const context::MessageNode*> chain = graph_.Chain();
  if (turn < 1 || turn > chain.size()) return false;
  const context::MessageId target = (turn == 1) ? context::MessageId{} : chain[turn - 2]->id;
  return graph_.RewindTo(target);
}

bool Conversation::HasPendingToolCalls() const { return graph_.LeafHasUnfinishedToolCalls(); }

void Conversation::ClearHistory() { graph_ = context::MessageGraph{}; }

boost::json::value Conversation::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["history"] = graph_.Serialize();
  return j;
}

std::shared_ptr<Conversation> Conversation::Deserialize(const boost::json::value& j) {
  auto conversation = std::make_shared<Conversation>();

  if (json::HasKey(j, "history")) {
    if (!context::MessageGraph::Deserialize(j.at("history"), conversation->graph_)) return nullptr;
  }

  return conversation;
}

Session::Session() : conversation_(std::make_shared<Conversation>()), spec_() {}

Session::Session(std::shared_ptr<Conversation> conversation, const SessionSpec& spec)
    : conversation_(std::move(conversation)), spec_(spec) {}

void Session::SetAgent(const std::string& agent_name) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
        "Cannot switch agent while tool calls are pending. "
        "Please let the current tool finish or /clear.");
  }
  // Choosing an agent drops the override, so the agent's own configuration
  // becomes the source of the backend again.
  spec_.agent_name = agent_name;
  spec_.backend_override.reset();
}

void Session::SetBackendOverride(const config::BackendConfig& new_config) {
  if (HasPendingToolCalls()) {
    throw RuntimeError(
        "Cannot switch backend while tool calls are pending. "
        "Please let the current tool finish or /clear.");
  }
  spec_.backend_override = new_config;
}

std::unique_ptr<LLMProvider> Session::CreateProvider(const config::BackendConfig& backend) const {
  return config::CreateBackend(backend, std::make_unique<pu::http::BeastHttpClient>());
}

boost::json::value Session::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["schema_version"] = kSessionSchemaVersion;
  j.as_object()["conversation"] = conversation_->Serialize();
  j.as_object()["session_spec"] = spec_.Serialize();
  return j;
}

std::unique_ptr<Session> Session::Deserialize(const boost::json::value& j) {
  const bool has_version = json::HasKey(j, "schema_version");
  const int version = json::ValueOrDefault<int>(j, "schema_version", 0);
  if (!has_version || version != kSessionSchemaVersion) return nullptr;

  // A version field alone is not enough: the storage itself has to look like node
  // storage, or the file is a layout this build cannot read.
  if (!json::HasKey(j, "conversation") || !json::HasKey(j.at("conversation"), "history") ||
      !j.at("conversation").at("history").is_object()) {
    return nullptr;
  }

  auto conversation = Conversation::Deserialize(j.at("conversation"));
  if (!conversation) return nullptr;

  // The spec is what selects the agent, so a file without it would load as a
  // conversation that cannot reach a model.
  if (!json::HasKey(j, "session_spec") || !j.at("session_spec").is_object()) {
    return nullptr;
  }
  std::optional<SessionSpec> spec = SessionSpec::Deserialize(j.at("session_spec"));
  if (!spec) return nullptr;

  return std::make_unique<Session>(conversation, *spec);
}

}  // namespace pu
