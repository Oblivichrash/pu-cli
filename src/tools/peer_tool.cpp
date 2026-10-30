// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/peer_tool.hpp"

#include "pu/core/json.hpp"
#include "pu/session/message.hpp"

#include <spdlog/spdlog.h>

#include <sstream>
#include <stdexcept>
#include <vector>

namespace pu::tools {

namespace {

constexpr const char* kPeerPath = "/api/peer/message";
constexpr const char* kJobsPath = "/api/jobs";

}  // namespace

PeerTool::PeerTool(const config::PeerConfig& peers, pu::http::HttpClient* client)
    : peers_(peers), client_(client) {}

std::string PeerTool::Name() const { return "send_to_peer"; }

std::string PeerTool::Description() const {
  return "Hand work to another pu server, and watch it on that server's task board.";
}

boost::json::value PeerTool::ParametersSchema() const {
  boost::json::array names;
  for (const auto& peer : peers_.peers) names.push_back(boost::json::string(peer.name));

  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"peer", boost::json::object{{"type", "string"},
                                        {"enum", std::move(names)},
                                        {"description", "The server to hand the work to"}}},
           {"intent",
            boost::json::object{
                {"type", "string"},
                {"description", "One line naming the work, shown on the board until it closes"}}},
           {"message",
            boost::json::object{{"type", "string"},
                                {"description", "What to say to the peer, in full"}}},
           {"due",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "Optional deadline as a duration such as \"30m\", \"2h\" or \"1d\"; the board "
                 "reports the job as overdue after it"}}},
       }},
      {"required", boost::json::array{"peer", "intent", "message"}},
  };
}

std::vector<std::string> PeerTool::Headers() const {
  std::vector<std::string> headers{"Content-Type: application/json"};
  if (!peers_.token.empty()) headers.push_back("Authorization: Bearer " + peers_.token);
  return headers;
}

PeerTool::PostedJob PeerTool::PostJob(const std::string& peer_url, const std::string& to,
                                      const std::string& intent,
                                      const std::optional<std::string>& due) {
  boost::json::object request{{"from", peers_.self_name}, {"to", to}, {"intent", intent}};
  if (due) request["due"] = *due;

  std::string received;
  client_->PostStream(peer_url + kJobsPath, json::serialize(request), Headers(),
                      [&](char* data, std::size_t size) -> std::size_t {
                        received.append(data, size);
                        return size;
                      });

  const boost::json::value reply = json::parse(received);
  const boost::json::value& job = reply.at("job");
  PostedJob posted;
  posted.id = json::ValueOrDefault<std::string>(job, "id", "");
  posted.due_at = json::ValueOrDefault<std::string>(job, "due_at", "");
  if (posted.id.empty()) throw std::runtime_error("the board did not return a job id");
  return posted;
}

boost::json::value PeerTool::Send(const std::string& peer_url, const std::string& message,
                                  const PostedJob& job) {
  const std::string body = json::serialize(boost::json::object{{"from", peers_.self_name},
                                                               {"message", message},
                                                               {"job", job.id},
                                                               {"hop", 1}});
  std::string received;
  client_->PostStream(peer_url + kPeerPath, body, Headers(),
                      [&](char* data, std::size_t size) -> std::size_t {
                        received.append(data, size);
                        return size;
                      });
  return json::parse(received);
}

std::string PeerTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  const std::string name = json::ValueOrDefault<std::string>(args, "peer", "");
  const std::string intent = json::ValueOrDefault<std::string>(args, "intent", "");
  const std::string message = json::ValueOrDefault<std::string>(args, "message", "");

  if (name.empty() || intent.empty() || message.empty()) {
    return MakeRefusal("A peer, an intent and a message are all required", true);
  }

  if (!ctx.peer_origin.empty() && name != ctx.peer_origin) {
    return MakeRefusal(std::string(session::kPeerOriginNote) + " (this turn was asked by '" +
                           ctx.peer_origin + "', so only '" + ctx.peer_origin +
                           "' can be reached)",
                       true);
  }

  const auto peer = peers_.FindPeer(name);
  if (!peer.has_value()) {
    return MakeRefusal("No peer named '" + name + "' is configured", true);
  }
  if (client_ == nullptr) {
    return MakeRefusal("No HTTP client is available to reach '" + name + "'");
  }

  const std::string due = json::ValueOrDefault<std::string>(args, "due", "");
  const std::optional<std::string> deadline = due.empty() ? std::nullopt : std::optional{due};

  PostedJob job;
  try {
    job = PostJob(peer->url, peer->name, intent, deadline);
  } catch (const std::exception& e) {
    spdlog::warn("Peer '{}' did not take the job: {}", name, e.what());
    return MakeRefusal("Peer '" + name + "' did not take the job, so nothing was sent: " +
                       e.what());
  }

  boost::json::value reply;
  try {
    reply = Send(peer->url, message, job);
  } catch (const std::exception& e) {
    // The job stays on the board, open and unseen. Saying so is what lets this turn or a
    // person pick it up rather than assume the work is under way.
    return MakeRefusal("Job '" + job.id + "' was posted on '" + name +
                       "' but the message did not arrive: " + e.what());
  }

  if (!json::ValueOrDefault<bool>(reply, "accepted", false)) {
    const std::string reason =
        json::ValueOrDefault<std::string>(reply, "error", "the peer did not say why");
    return MakeRefusal("Job '" + job.id + "' was posted but '" + name + "' refused it: " + reason);
  }

  const std::string turn = json::ValueOrDefault<std::string>(reply, "turn", "");
  const std::string model = json::ValueOrDefault<std::string>(reply, "model", "");

  std::ostringstream said;
  said << "Job '" << job.id << "' is on '" << name << "'s board and the turn is "
       << (turn.empty() ? std::string("queued") : "'" + turn + "'");
  if (!job.due_at.empty()) said << ", due " << job.due_at;
  if (!model.empty()) said << ", running as " << model;
  said << ". It answers in a later turn, so do not wait for it or ask again.";
  return MakeToolResultJson(true, said.str(), "", 0);
}

}  // namespace pu::tools
