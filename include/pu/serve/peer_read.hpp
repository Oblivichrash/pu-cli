// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Reading a peer's board, which is also how that peer learns this one is still working.
//
// A pull carries the token, so "last heard from" is a claim nobody can make without the secret:
// the old heartbeat took the peer's name from the request body, which proved only that someone
// had sent a string. And the pull is made from inside the turn loop, so the interval it is seen
// at is the interval work is being done at — a wedged agent stops being heard from by simply
// stopping.

#include "pu/config/peer.hpp"
#include "pu/core/http_client.hpp"

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pu::serve {

// What one pull answered.
struct PeerSnapshot {
  std::string peer;
  boost::json::value board;
  // Set when the pull failed, so a caller can say why the board is missing rather than draw an
  // empty one as if the peer had nothing to do.
  std::string error;
};

class BoardReader {
 public:
  BoardReader(config::PeerConfig peers, http::HttpClient* client)
      : peers_(std::move(peers)), client_(client) {}

  // Pulls every configured peer's board. A peer that does not answer is reported with its
  // error rather than dropped, so a digest can say "B could not be reached" instead of
  // silently showing nothing.
  std::vector<PeerSnapshot> PullAll();

  // The boards as one block of text for the model, empty when nothing was read and nothing
  // failed. `peers_` is what is configured, `self_name` what the peers recorded reading.
  std::string Describe(const std::vector<PeerSnapshot>& snapshots) const;

 private:
  std::optional<PeerSnapshot> PullOne(const config::PeerEntry& peer);

  config::PeerConfig peers_;
  http::HttpClient* client_;
};

}  // namespace pu::serve
