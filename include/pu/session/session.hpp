// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/backend.hpp"
#include "pu/context/graph.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

// The stored conversation: what was said, in order. Storage behind it is the
// MessageGraph (include/pu/context/graph.hpp), and the compatibility seam is the
// ChatMessage view this class renders from it.
class Conversation {
 public:
  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const;
  bool HasPendingToolCalls() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return graph_; }

  // Moves the position back to just before the 1-based turn. Every stored node
  // stays until the next append replaces the turns after the new position.
  bool RewindBefore(size_t turn);

  void ClearHistory();

  boost::json::value Serialize() const;
  // Null for a value that is not node storage, so the caller can tell a foreign
  // layout from an empty conversation.
  static std::shared_ptr<Conversation> Deserialize(const boost::json::value& j);

 private:
  context::MessageGraph graph_;
};

// What a session names and carries: the agent it is talking to, and a backend only
// when one was chosen for it. Everything else about the backend is read from
// agents.json, so editing the configuration takes effect on restart.
struct SessionSpec {
  std::string agent_name;
  std::optional<config::BackendConfig> backend_override;
  // The thinking level this session asks for. Absent means it follows whatever the
  // agent's configuration says, which is a different thing from the absent level:
  // that one asks the backend to decide.
  std::optional<ThinkingLevel> thinking_override;

  boost::json::value Serialize() const {
    boost::json::value jv = {{"agent_name", agent_name}};
    if (backend_override) {
      jv.as_object()["backend_override"] = boost::json::value_from(*backend_override);
    }
    if (thinking_override) {
      jv.as_object()["thinking_override"] = ThinkingLevelName(*thinking_override);
    }
    return jv;
  }

  // A section that is not an object cannot name an agent, so it is refused
  // rather than read into a spec that would start the wrong model.
  static std::optional<SessionSpec> Deserialize(const boost::json::value& jv) {
    if (!jv.is_object()) return std::nullopt;

    SessionSpec spec;
    if (json::HasKey(jv, "backend_override")) {
      spec.backend_override =
          boost::json::value_to<config::BackendConfig>(jv.at("backend_override"));
    }
    if (json::HasKey(jv, "thinking_override") && jv.at("thinking_override").is_string()) {
      spec.thinking_override =
          ParseThinkingLevel(boost::json::value_to<std::string>(jv.at("thinking_override")));
    }
    spec.agent_name = json::ValueOrDefault<std::string>(jv, "agent_name", "");
    return spec;
  }
};

// The version this build writes into a session file, and the only one it reads back. A
// file carrying another one is refused rather than guessed at. It lives here because the
// file is a session: the conversation inside it is storage without a version of its own.
inline constexpr int kSessionSchemaVersion = 5;

// Aggregate root: the conversation, plus the agent and backend it belongs to.
class Session {
 public:
  Session();
  Session(std::shared_ptr<Conversation> conversation, const SessionSpec& spec);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = default;
  Session& operator=(Session&&) = default;

  Conversation& GetConversation() { return *conversation_; }
  const Conversation& GetConversation() const { return *conversation_; }
  SessionSpec& GetSpec() { return spec_; }
  const SessionSpec& GetSpec() const { return spec_; }

  void SetBackendOverride(const config::BackendConfig& new_config);
  // Choosing an agent drops the override, so the agent's own configuration
  // becomes the source of the backend again.
  void SetAgent(const std::string& agent_name);

  bool HasPendingToolCalls() const { return conversation_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider(const config::BackendConfig& backend) const;

  boost::json::value Serialize() const;
  static std::unique_ptr<Session> Deserialize(const boost::json::value& j);

 private:
  std::shared_ptr<Conversation> conversation_;
  SessionSpec spec_;
};

}  // namespace pu
