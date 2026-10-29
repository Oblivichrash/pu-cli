// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/backend.hpp"
#include "pu/session/graph.hpp"
#include "pu/session/settings.hpp"
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
  // Whether the turn at `turn` (1-based, as /api/rewind counts) was asked by a peer. Such a
  // turn belongs to the conversation it arrived in and cannot be taken back from this page.
  bool TurnCameFromPeer(size_t turn) const;

  void ClearHistory();

  boost::json::value Serialize() const;
  static std::shared_ptr<Conversation> Deserialize(const boost::json::value& j);

 private:
  session::MessageGraph graph_;
};

struct SessionSpec {
  std::string agent_name;

  // What this session overrides about the agent's configuration, by setting name. The names and
  // the values both come from the table in `pu/session/settings.hpp`: a name that is not in it
  // is never stored, and a value is checked before it gets here.
  //
  // It is a map rather than a field per setting so that `tool_budget` did not have to be the
  // second of eight copies: one row in that table is the whole of adding one.
  session::Overrides overrides;

  // The `thinking` override, for the callers that only know about that one.
  std::optional<ThinkingLevel> ThinkingOverride() const {
    const auto it = overrides.find("thinking");
    if (it == overrides.end() || !it->value().is_string()) return std::nullopt;
    return ParseThinkingLevel(boost::json::value_to<std::string>(it->value()));
  }

  boost::json::value Serialize() const {
    boost::json::value jv = {{"agent_name", agent_name}};
    for (const auto& member : overrides) {
      if (member.value().is_null()) continue;
      jv.as_object()[std::string(member.key()) + "_override"] = member.value();
    }
    return jv;
  }

  static std::optional<SessionSpec> Deserialize(const boost::json::value& jv) {
    if (!jv.is_object()) return std::nullopt;

    SessionSpec spec;
    spec.agent_name = json::ValueOrDefault<std::string>(jv, "agent_name", "");
    // The stored names carry `_override`, which is the file's spelling of "this session chose
    // it"; the table's names do not.
    for (const auto& member : jv.as_object()) {
      const std::string key(member.key());
      constexpr std::string_view kSuffix = "_override";
      if (key.size() <= kSuffix.size()) continue;
      if (key.compare(key.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) continue;
      const std::string name = key.substr(0, key.size() - kSuffix.size());
      spec.overrides[name] = member.value();
    }
    return spec;
  }
};

// 9: a tool receipt carries `origin` — the question, the run and its rank — so a projection
// reads where a page came from instead of parsing its text on every turn.
inline constexpr int kSessionSchemaVersion = 9;

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
