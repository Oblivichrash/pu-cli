// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/graph.hpp"

#include "pu/core/json.hpp"

namespace pu::session {

namespace {

constexpr const char* kStatusPending = kPendingStatusName;
constexpr const char* kStatusCompleted = kCompletedStatusName;
constexpr const char* kStatusInterrupted = kInterruptedStatusName;

ToolCallStatus StatusFrom(const std::string& name) {
  if (name == kStatusCompleted) return ToolCallStatus::kCompleted;
  if (name == kStatusInterrupted) return ToolCallStatus::kInterrupted;
  return ToolCallStatus::kPending;
}

boost::json::value SerializeNode(const MessageNode& node) {
  boost::json::object out = {
      {"id", node.id},
      {"timestamp", node.timestamp},
      {"parent", node.parent},
  };

  if (const auto* user = std::get_if<UserPayload>(&node.payload)) {
    out["role"] = kUserRole;
    out["content"] = user->content;
    if (!user->from_peer.empty()) out["from_peer"] = user->from_peer;
  } else if (const auto* assistant = std::get_if<AssistantPayload>(&node.payload)) {
    out["role"] = kAssistantRole;
    out["content"] = assistant->content;
    if (!assistant->reasoning.empty()) {
      boost::json::array blocks;
      for (const Reasoning& block : assistant->reasoning) {
        blocks.push_back(ReasoningToJson(block));
      }
      out["reasoning"] = std::move(blocks);
    }
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(boost::json::value{
            {"id", record.id},
            {"name", record.name},
            {"arguments", record.arguments},
            {"status", ToolCallStatusName(record.status)},
        });
      }
      out["tool_calls"] = std::move(calls);
    }
  } else if (const auto* system = std::get_if<SystemPayload>(&node.payload)) {
    out["role"] = kSystemRole;
    out["content"] = system->content;
  } else {
    const auto& receipt = std::get<ToolPayload>(node.payload);
    out["role"] = kToolRole;
    out["content"] = receipt.content;
    out["tool_call_id"] = receipt.tool_call_id;
    out["tool_name"] = receipt.tool_name;
  }

  return out;
}

bool DeserializeNode(const boost::json::value& value, MessageNode& out) {
  if (!value.is_object()) return false;

  out.id = json::ValueOrDefault<std::string>(value, "id", "");
  if (out.id.empty()) return false;
  out.timestamp = json::ValueOrDefault<std::string>(value, "timestamp", "");
  if (json::HasKey(value, "parent")) {
    if (!value.at("parent").is_string()) return false;
    out.parent = boost::json::value_to<std::string>(value.at("parent"));
  }

  const std::string role = json::ValueOrDefault<std::string>(value, "role", "");
  const std::string content = json::ValueOrDefault<std::string>(value, "content", "");

  if (role == kUserRole) {
    UserPayload user;
    user.content = content;
    user.from_peer = json::ValueOrDefault<std::string>(value, "from_peer", "");
    out.payload = std::move(user);
    return true;
  }
  if (role == kAssistantRole) {
    AssistantPayload assistant;
    assistant.content = content;
    if (json::HasKey(value, "reasoning") && value.at("reasoning").is_array()) {
      for (const boost::json::value& block : value.at("reasoning").as_array()) {
        assistant.reasoning.push_back(ReasoningFromJson(block));
      }
    }
    if (json::HasKey(value, "tool_calls") && value.at("tool_calls").is_array()) {
      for (const boost::json::value& call : value.at("tool_calls").as_array()) {
        ToolCallRecord record;
        record.id = json::ValueOrDefault<std::string>(call, "id", "");
        record.name = json::ValueOrDefault<std::string>(call, "name", "");
        record.arguments =
            json::ValueOrDefault<boost::json::value>(call, "arguments", boost::json::object{});
        record.status = StatusFrom(json::ValueOrDefault<std::string>(call, "status", ""));
        assistant.tool_calls.push_back(std::move(record));
      }
    }
    out.payload = std::move(assistant);
    return true;
  }
  if (role == kSystemRole) {
    SystemPayload system;
    system.content = content;
    out.payload = std::move(system);
    return true;
  }
  if (role == kToolRole) {
    ToolPayload receipt;
    receipt.content = content;
    receipt.tool_call_id = json::ValueOrDefault<std::string>(value, "tool_call_id", "");
    receipt.tool_name = json::ValueOrDefault<std::string>(value, "tool_name", "");
    out.payload = std::move(receipt);
    return true;
  }
  return false;
}

}  // namespace

boost::json::value MessageGraph::Serialize() const {
  boost::json::array nodes;
  for (const auto& [id, node] : nodes_) nodes.push_back(SerializeNode(node));

  return boost::json::value{
      {"nodes", std::move(nodes)},
      {"leaf", leaf_},
  };
}

bool MessageGraph::Deserialize(const boost::json::value& value, MessageGraph& out) {
  if (!json::HasKey(value, "nodes") || !value.at("nodes").is_array()) return false;
  if (!json::HasKey(value, "leaf") || !value.at("leaf").is_string()) return false;

  MessageGraph graph;
  for (const boost::json::value& entry : value.at("nodes").as_array()) {
    MessageNode node;
    if (!DeserializeNode(entry, node)) return false;
    if (!graph.nodes_.emplace(node.id, std::move(node)).second) return false;
  }

  const MessageId leaf = boost::json::value_to<std::string>(value.at("leaf"));
  if (!leaf.empty() && graph.nodes_.find(leaf) == graph.nodes_.end()) return false;

  if (!graph.LinksResolve()) return false;

  graph.leaf_ = leaf;
  out = std::move(graph);
  return true;
}

}  // namespace pu::session
