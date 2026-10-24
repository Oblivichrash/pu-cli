// SPDX-License-Identifier: GPL-3.0-only
#include "pu/serve/peer_read.hpp"

#include <spdlog/spdlog.h>

#include <sstream>

#include "pu/core/json.hpp"

namespace pu::serve {

namespace {

namespace json = pu::json;

std::string StringField(const boost::json::value& value, const char* key) {
  return json::ValueOrDefault<std::string>(value, key, "");
}

}  // namespace

std::optional<PeerSnapshot> BoardReader::PullOne(const config::PeerEntry& peer) {
  if (client_ == nullptr) return std::nullopt;

  std::vector<std::string> headers{"Accept: application/json"};
  if (!peers_.token.empty()) headers.push_back("Authorization: Bearer " + peers_.token);

  PeerSnapshot snapshot;
  snapshot.peer = peer.name;
  try {
    snapshot.board = json::parse(client_->Get(peer.url + "/api/jobs", headers).body);
  } catch (const std::exception& e) {
    // A peer that cannot be read is reported, not dropped: "B could not be reached" is a fact
    // the model can act on, an empty board is a lie about B having nothing to do.
    spdlog::debug("Peer '{}' board could not be read: {}", peer.name, e.what());
    snapshot.error = e.what();
  }
  return snapshot;
}

std::vector<PeerSnapshot> BoardReader::PullAll() {
  std::vector<PeerSnapshot> snapshots;
  for (const config::PeerEntry& peer : peers_.peers) {
    if (auto one = PullOne(peer); one.has_value()) snapshots.push_back(std::move(*one));
  }
  return snapshots;
}

namespace {

// One job, as the model reads it. Only what it can act on: the id it would move, who owes it,
// what state it is in, and the question if one is being asked.
void AppendJob(std::ostringstream& out, const boost::json::value& job, const std::string& self) {
  const std::string to = StringField(job, "to");
  const std::string from = StringField(job, "from");
  const bool mine = to == self;
  const bool theirs = from == self;

  out << "  - " << StringField(job, "id") << " " << (mine ? "you hold" : "held by " + to)
      << ", " << (theirs ? "you posted" : "posted by " + (from.empty() ? "a person" : from))
      << ": " << StringField(job, "intent") << " [" << StringField(job, "state");
  if (json::HasKey(job, "overdue") && job.at("overdue").is_bool() && job.at("overdue").as_bool()) {
    out << ", overdue";
  }
  if (json::HasKey(job, "question") && job.at("question").is_string()) {
    out << ", asking: " << boost::json::value_to<std::string>(job.at("question"));
  }
  if (json::HasKey(job, "result") && job.at("result").is_string()) {
    out << ", result: " << boost::json::value_to<std::string>(job.at("result"));
  }
  out << "]\n";
}

}  // namespace

std::string BoardReader::Describe(const std::vector<PeerSnapshot>& snapshots) const {
  const std::string self = peers_.self_name;
  std::ostringstream out;
  bool any = false;

  for (const PeerSnapshot& snapshot : snapshots) {
    if (!snapshot.error.empty()) {
      out << "  - " << snapshot.peer << " could not be read: " << snapshot.error << "\n";
      any = true;
      continue;
    }
    const boost::json::value& board = snapshot.board;
    if (!board.is_object()) continue;

    // How long ago this peer last read *this* server's board, which is what it means for the
    // peer to have seen a job handed to it. Absent until it has read once.
    std::string seen;
    if (json::HasKey(board, "self") && board.at("self").is_object()) {
      const auto& presence = board.at("self");
      if (json::HasKey(presence, "seen_seconds")) {
        seen = std::to_string(json::ValueOrDefault<std::int64_t>(presence, "seen_seconds", 0));
      }
    }

    out << "  - " << snapshot.peer << ":";
    if (!seen.empty()) out << " your board was last read by it " << seen << "s ago;";
    out << "\n";
    any = true;

    if (!json::HasKey(board, "jobs") || !board.at("jobs").is_array()) continue;
    for (const boost::json::value& job : board.at("jobs").as_array()) {
      if (!job.is_object()) continue;
      AppendJob(out, job, self);
    }
  }

  if (!any) return {};
  std::ostringstream headed;
  headed << "=== Peer boards ===\n" << out.str();
  return headed.str();
}

}  // namespace pu::serve
