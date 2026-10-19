// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>

#include <boost/json.hpp>

#include "pu/config/peer.hpp"
#include "pu/core/http_client.hpp"
#include "pu/tools/tool.hpp"

namespace pu::tools {

// Hands work to another pu server. The work is posted on the receiver's task board first and
// only then sent: a board that refuses the job stops the call before anything is said, so a
// message never arrives at a server that has no record of why it came. The post is what makes
// a handed-off turn survivable - it outlives the sender's turn, and whoever watches the board
// can see it claimed, closed, or left behind.
class PeerTool : public pu::Tool {
 public:
  PeerTool(const config::PeerConfig& peers, pu::http::HttpClient* client);

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  // What the receiver's board made of the job: the id to name in the message and the deadline
  // it was given, which the sender reports back rather than recomputing.
  struct PostedJob {
    std::string id;
    std::string due_at;
  };

  std::vector<std::string> Headers() const;
  // Posts the job, or throws with the board's own words. A refusal here is final: nothing is
  // sent, and the sender's turn is told why.
  PostedJob PostJob(const std::string& peer_url, const std::string& to, const std::string& intent,
                    const std::optional<std::string>& due);
  // Sends the message under the job, returning the receiver's acceptance body.
  boost::json::value Send(const std::string& peer_url, const std::string& message,
                          const PostedJob& job);

  const config::PeerConfig& peers_;
  pu::http::HttpClient* client_;
};

}  // namespace pu::tools
