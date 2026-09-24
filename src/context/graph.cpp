// SPDX-License-Identifier: GPL-3.0-only
#include "pu/context/graph.hpp"

#include "pu/core/json.hpp"

namespace pu::context {

namespace {

constexpr const char* kStatusPending = "pending";
constexpr const char* kStatusCompleted = "completed";

std::string StatusName(ToolCallStatus status) {
  switch (status) {
    case ToolCallStatus::kPending:
      return kStatusPending;
    case ToolCallStatus::kCompleted:
      return kStatusCompleted;
  }
  return kStatusPending;
}

ToolCallStatus StatusFrom(const std::string& name) {
  if (name == kStatusCompleted) return ToolCallStatus::kCompleted;
  return ToolCallStatus::kPending;
}

boost::json::array SerializeContent(const std::vector<ContentPart>& content) {
  boost::json::array parts;
  for (const ContentPart& part : content) {
    parts.push_back(boost::json::value{
        {"type", "text"},
        {"text", std::get<TextPart>(part).text},
    });
  }
  return parts;
}

std::vector<ContentPart> DeserializeContent(const boost::json::value& value) {
  std::vector<ContentPart> content;
  if (!value.is_array()) return content;
  for (const boost::json::value& part : value.as_array()) {
    // Only text exists today; an unknown part type is skipped rather than
    // turned into an empty text part, so the loss is visible in the count.
    if (json::ValueOrDefault<std::string>(part, "type", "") == "text") {
      content.emplace_back(TextPart{json::ValueOrDefault<std::string>(part, "text", "")});
    }
  }
  return content;
}

boost::json::value SerializeNode(const MessageNode& node) {
  boost::json::array parents;
  for (const MessageId& parent : node.parents) {
    parents.push_back(boost::json::value(parent));
  }
  boost::json::object out = {
      {"id", node.id},
      {"timestamp", node.timestamp},
      {"parents", std::move(parents)},
  };

  if (const auto* user = std::get_if<UserPayload>(&node.payload)) {
    out["role"] = kUserRole;
    out["content"] = SerializeContent(user->content);
  } else if (const auto* assistant = std::get_if<AssistantPayload>(&node.payload)) {
    out["role"] = kAssistantRole;
    out["content"] = SerializeContent(assistant->content);
    if (assistant->reasoning) {
      out["reasoning"] = boost::json::value{
          {"provider", assistant->reasoning->provider},
          {"signature", assistant->reasoning->signature},
          {"raw_json", assistant->reasoning->raw_json},
      };
    }
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(boost::json::value{
            {"id", record.id},
            {"name", record.name},
            {"arguments", record.arguments},
            {"status", StatusName(record.status)},
        });
      }
      out["tool_calls"] = std::move(calls);
    }
  } else if (const auto* system = std::get_if<SystemPayload>(&node.payload)) {
    out["role"] = kSystemRole;
    out["content"] = SerializeContent(system->content);
  } else {
    const auto& receipt = std::get<ToolPayload>(node.payload);
    out["role"] = kToolRole;
    out["content"] = SerializeContent(receipt.content);
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
  if (json::HasKey(value, "parents") && value.at("parents").is_array()) {
    for (const boost::json::value& parent : value.at("parents").as_array()) {
      // A parent that is not a name cannot point at a node, so the file is
      // refused rather than loaded with a node that lost its place in the chain.
      if (!parent.is_string()) return false;
      out.parents.push_back(boost::json::value_to<std::string>(parent));
    }
  }

  const std::string role = json::ValueOrDefault<std::string>(value, "role", "");
  const std::vector<ContentPart> content = json::HasKey(value, "content")
                                               ? DeserializeContent(value.at("content"))
                                               : std::vector<ContentPart>{};

  if (role == kUserRole) {
    UserPayload user;
    user.content = content;
    out.payload = std::move(user);
    return true;
  }
  if (role == kAssistantRole) {
    AssistantPayload assistant;
    assistant.content = content;
    if (json::HasKey(value, "reasoning") && value.at("reasoning").is_object()) {
      const boost::json::value& reasoning = value.at("reasoning");
      assistant.reasoning = Reasoning{
          json::ValueOrDefault<std::string>(reasoning, "provider", ""),
          json::ValueOrDefault<std::string>(reasoning, "signature", ""),
          json::ValueOrDefault<std::string>(reasoning, "raw_json", ""),
      };
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
  // Sorted by id so the file is stable for the same conversation, which makes a
  // diff meaningful and a test that compares two saves meaningful as well.
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
    graph.nodes_.emplace(node.id, std::move(node));
  }

  const MessageId leaf = boost::json::value_to<std::string>(value.at("leaf"));
  // A leaf that names nothing would leave the graph unreadable, so a file in
  // that state is refused rather than loaded as empty.
  if (!leaf.empty() && graph.nodes_.find(leaf) == graph.nodes_.end()) return false;

  graph.leaf_ = leaf;
  out = std::move(graph);
  return true;
}

}  // namespace pu::context
