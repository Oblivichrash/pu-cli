// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/backend.hpp"
#include "pu/session/graph.hpp"
#include "pu/session/usage.hpp"
#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class Conversation {
 public:
  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  // Callers that need both the rendered text and the node-level fields must read the
  // nodes once through here; History() renders a separate list and pairing the two
  // by index would rely on an ordering neither one guarantees.
  std::vector<ChatMessage> History() const;
  std::vector<const session::MessageNode*> Chain() const { return graph_.Chain(); }
  bool HasPendingToolCalls() const;

  void SealInterruptedToolCalls();

  const session::MessageGraph& Graph() const { return graph_; }

  bool RewindBefore(size_t turn);

  void ClearHistory();

  boost::json::value Serialize() const;
  static std::shared_ptr<Conversation> Deserialize(const boost::json::value& j);

 private:
  session::MessageGraph graph_;
};

struct SessionSpec {
  std::string agent_name;
  std::optional<ThinkingLevel> thinking_override;

  boost::json::value Serialize() const {
    boost::json::value jv = {{"agent_name", agent_name}};
    if (thinking_override) {
      jv.as_object()["thinking_override"] = ThinkingLevelName(*thinking_override);
    }
    return jv;
  }

  static std::optional<SessionSpec> Deserialize(const boost::json::value& jv) {
    if (!jv.is_object()) return std::nullopt;

    SessionSpec spec;
    if (json::HasKey(jv, "thinking_override") && jv.at("thinking_override").is_string()) {
      spec.thinking_override =
          ParseThinkingLevel(boost::json::value_to<std::string>(jv.at("thinking_override")));
    }
    spec.agent_name = json::ValueOrDefault<std::string>(jv, "agent_name", "");
    return spec;
  }
};

inline constexpr int kSessionSchemaVersion = 8;

class Session {
 public:
  Session();
  Session(std::shared_ptr<Conversation> conversation, const SessionSpec& spec);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = default;
  Session& operator=(Session&&) = default;

  Conversation& Messages() { return *conversation_; }
  const Conversation& Messages() const { return *conversation_; }
  SessionSpec& Spec() { return spec_; }
  const SessionSpec& Spec() const { return spec_; }

  // What each turn cost, kept beside the conversation rather than in it: a model never reads
  // it, and a page draws it next to the reply rather than as part of it.
  session::UsageLedger& Usage() { return usage_; }
  const session::UsageLedger& Usage() const { return usage_; }

  void SetAgent(const std::string& agent_name);

  bool HasPendingToolCalls() const { return conversation_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider(const config::Backend& backend,
                                              std::shared_ptr<http::HttpClient> http) const;

  boost::json::value Serialize() const;
  static std::unique_ptr<Session> Deserialize(const boost::json::value& j);

 private:
  std::shared_ptr<Conversation> conversation_;
  SessionSpec spec_;
  session::UsageLedger usage_;
};

}  // namespace pu
