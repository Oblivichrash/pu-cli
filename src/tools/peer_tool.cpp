// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/peer_tool.hpp"

#include "pu/core/json.hpp"
#include "pu/session/message.hpp"

#include <spdlog/spdlog.h>

#include <sstream>

namespace pu::tools {

namespace {

constexpr const char* kPeerPath = "/api/peer/message";

}  // namespace

PeerTool::PeerTool(const config::PeerConfig& peers, pu::http::HttpClient* client)
    : peers_(peers), client_(client) {}

std::string PeerTool::Name() const { return "send_to_peer"; }

std::string PeerTool::Description() const { return "Send a message to another pu server."; }

boost::json::value PeerTool::ParametersSchema() const {
  boost::json::array names;
  for (const auto& peer : peers_.peers) names.push_back(boost::json::string(peer.name));

  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"peer", boost::json::object{{"type", "string"},
                                        {"enum", std::move(names)},
                                        {"description", "The peer to send the message to"}}},
           {"message",
            boost::json::object{{"type", "string"},
                                {"description", "What to say to the peer"}}},
       }},
      {"required", boost::json::array{"peer", "message"}},
  };
}

std::string PeerTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  const std::string name = json::ValueOrDefault<std::string>(args, "peer", "");
  const std::string message = json::ValueOrDefault<std::string>(args, "message", "");

  if (name.empty() || message.empty()) {
    return MakeToolResultJson(false, "", "A peer and a message are both required", -1, true);
  }

  if (!ctx.peer_origin.empty() && name != ctx.peer_origin) {
    return MakeToolResultJson(false, "",
                              std::string(session::kPeerOriginNote) + " (this turn was asked by '" +
                                  ctx.peer_origin + "', so only '" + ctx.peer_origin +
                                  "' can be reached)",
                              -1, true);
  }

  const auto peer = peers_.FindPeer(name);
  if (!peer.has_value()) {
    return MakeToolResultJson(false, "", "No peer named '" + name + "' is configured", -1, true);
  }
  if (client_ == nullptr) {
    return MakeToolResultJson(false, "", "No HTTP client is available to reach '" + name + "'", -1);
  }

  const std::string body =
      json::serialize(boost::json::object{{"from", peers_.self_name},
                                          {"message", message},
                                          {"hop", 1}});
  std::vector<std::string> headers{"Content-Type: application/json"};
  if (!peers_.token.empty()) headers.push_back("Authorization: Bearer " + peers_.token);

  std::string received;
  try {
    client_->PostStream(peer->url + kPeerPath, body, headers,
                        [&](char* data, std::size_t size) -> std::size_t {
                          received.append(data, size);
                          return size;
                        });
  } catch (const std::exception& e) {
    spdlog::warn("Peer '{}' did not accept the message: {}", name, e.what());
    return MakeToolResultJson(false, "", "Peer '" + name + "' could not be reached: " + e.what(),
                              -1);
  }

  boost::json::value reply;
  try {
    reply = json::parse(received);
  } catch (const std::exception&) {
    return MakeToolResultJson(false, "", "Peer '" + name + "' answered with a body that is not JSON",
                              -1);
  }

  if (!json::ValueOrDefault<bool>(reply, "accepted", false)) {
    const std::string reason =
        json::ValueOrDefault<std::string>(reply, "error", "the peer did not say why");
    return MakeToolResultJson(false, "", "Peer '" + name + "' refused the message: " + reason, -1);
  }

  return MakeToolResultJson(true,
                            "'" + name + "' has the message. It will answer in a later turn, so do "
                                        "not wait for it or ask again",
                            "", 0);
}

}  // namespace pu::tools
