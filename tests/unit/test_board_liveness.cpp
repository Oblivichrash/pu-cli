// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/serve/board.hpp"
#include "tests/mocks/peer_harness.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <string>

using pu::serve::Board;
using pu::serve::JobRequest;
using pu::tests::PeerServer;
using pu::tests::WritePeersFile;

namespace {

JobRequest Offer(const std::string& to, const std::string& intent) {
  JobRequest request;
  request.to = to;
  request.intent = intent;
  return request;
}

}  // namespace

// Reading a peer's board is how that peer hears from the reader: the token names the peer, and
// the read itself is the record. The board answers with it in the same round trip.
TEST_CASE("Reading a peer's board records that peer as heard from") {
  PeerServer a("A", boost::json::object{{"content", "hi"}}, "stop");
  PeerServer b("B", boost::json::object{{"content", "hi"}}, "stop");
  WritePeersFile(a.Home(), "A", a.Port(), "B", b.Port());
  WritePeersFile(b.Home(), "B", b.Port(), "A", a.Port());

  b.Start();
  a.Start();

  // A reads B's board, so B's board now knows A was here.
  const auto read = a.Client().GetRaw("/api/jobs");
  CHECK(read.status == 200);

  const auto board = boost::json::parse(b.Client().GetRaw("/api/jobs").body);
  REQUIRE(board.at("peers").as_array().size() == 1);
  const boost::json::value& presence = board.at("peers").as_array().front();
  CHECK(boost::json::value_to<std::string>(presence.at("name")) == "B");
  CHECK(presence.at("online") == true);
  CHECK(presence.as_object().contains("seen_seconds"));
  CHECK(boost::json::value_to<std::int64_t>(presence.at("seen_seconds")) < 45);

  a.Stop();
  b.Stop();
}

// The audit trail is bounded rather than growing with the board's lifetime. Each job is closed
// as it is posted, since an open one would hit the board's limit long before the trail fills.
TEST_CASE("The board keeps a bounded audit trail") {
  Board board;
  for (int i = 0; i < 200; ++i) {
    const std::string id = board.Post(Offer("B", "job " + std::to_string(i))).job.id;
    JobRequest close;
    close.job = id;
    close.from = "B";
    close.state = "done";
    board.Update(close);
  }

  const auto snapshot = board.Snapshot(std::chrono::seconds{45});
  const auto& audit = snapshot.at("audit").as_array();
  CHECK(audit.size() <= Board::kMaxAudit);
  CHECK(audit.size() > 0);
}

// The board stops taking work once it holds its limit, rather than growing without bound.
TEST_CASE("A board at its limit refuses new work") {
  Board board;
  for (std::size_t i = 0; i < Board::kMaxOpen; ++i) {
    board.Post(Offer("B", "job " + std::to_string(i)));
  }
  CHECK_THROWS(board.Post(Offer("B", "one more")));
}
