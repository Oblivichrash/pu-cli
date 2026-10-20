// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/serve/peer_read.hpp"
#include "tests/mocks/mock_http_client.hpp"

#include <boost/json.hpp>

#include <string>
#include <vector>

using pu::config::PeerConfig;
using pu::config::PeerEntry;
using pu::serve::BoardReader;
using pu::tests::MakeMockHttpClient;

namespace {

PeerConfig TwoPeers() {
  PeerConfig peers;
  peers.self_name = "A";
  peers.token = "shared-secret";
  peers.peers = {PeerEntry{"B", "http://127.0.0.1:8912"}, PeerEntry{"C", "http://127.0.0.1:8913"}};
  return peers;
}

// One board as a peer answers it: the jobs it holds and what it recorded about the reader.
boost::json::value BoardAnswer(const std::string& self, std::int64_t seen_seconds) {
  boost::json::object presence;
  presence["name"] = self;
  presence["seen_seconds"] = seen_seconds;

  boost::json::object job;
  job["id"] = "job-1";
  job["from"] = "A";
  job["to"] = self;
  job["intent"] = "check the build";
  job["state"] = "working";

  return boost::json::object{
      {"success", true},
      {"jobs", boost::json::array{job}},
      {"peers", boost::json::array{presence}},
      {"self", std::move(presence)},
  };
}

}  // namespace

// The pull is the one call that proves a peer is alive, so it has to carry the token: without it
// every peer answers 403 and the digest silently reads as an empty board.
TEST_CASE("a pull carries the shared token", "[serve][board][reader]") {
  auto [mock, ptr] = MakeMockHttpClient();
  ptr->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, std::string("{}")};
  };

  BoardReader reader(TwoPeers(), ptr);
  reader.PullAll();

  REQUIRE_FALSE(ptr->last_headers.empty());
  bool has_token = false;
  for (const std::string& header : ptr->last_headers) {
    if (header == "Authorization: Bearer shared-secret") has_token = true;
  }
  CHECK(has_token);
  // The read is what /api/jobs answers, not another route.
  CHECK(ptr->last_get_url == "http://127.0.0.1:8913/api/jobs");
}

// A peer that cannot be read is reported as such. An empty board would say the peer has nothing
// to do, which is a different fact from "nobody answered".
TEST_CASE("a peer that cannot be read is reported, not dropped", "[serve][board][reader]") {
  auto [mock, ptr] = MakeMockHttpClient();
  int calls = 0;
  ptr->simulate_get = [&calls](const std::string& url, const std::vector<std::string>&) {
    ++calls;
    if (url.find("8912") != std::string::npos) throw pu::HttpError("connect: Connection refused");
    return pu::http::Response{200, boost::json::serialize(BoardAnswer("C", 3))};
  };

  BoardReader reader(TwoPeers(), ptr);
  const std::vector<pu::serve::PeerSnapshot> snapshots = reader.PullAll();

  REQUIRE(snapshots.size() == 2);
  CHECK(calls == 2);
  CHECK(snapshots[0].peer == "B");
  CHECK_FALSE(snapshots[0].error.empty());
  CHECK(snapshots[1].peer == "C");
  CHECK(snapshots[1].error.empty());

  const std::string described = reader.Describe(snapshots);
  CHECK(described.find("B could not be read") != std::string::npos);
  CHECK(described.find("Connection refused") != std::string::npos);
}

// What the model reads: the job it holds, who posted it, its state, and how long ago the peer
// read this server's board — which is what "did it see the job I handed it" means.
TEST_CASE("a pull describes the jobs and when the peer last read this board",
          "[serve][board][reader]") {
  auto [mock, ptr] = MakeMockHttpClient();
  ptr->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, boost::json::serialize(BoardAnswer("B", 7))};
  };

  PeerConfig one;
  one.self_name = "A";
  one.token = "shared-secret";
  one.peers = {PeerEntry{"B", "http://127.0.0.1:8912"}};

  BoardReader reader(one, ptr);
  const std::string described = reader.Describe(reader.PullAll());

  CHECK(described.find("=== Peer boards ===") != std::string::npos);
  CHECK(described.find("B:") != std::string::npos);
  CHECK(described.find("7s ago") != std::string::npos);
  CHECK(described.find("job-1") != std::string::npos);
  CHECK(described.find("check the build") != std::string::npos);
  CHECK(described.find("working") != std::string::npos);
}

// A session with no peers in play reads as it did before the board existed: nothing to say.
TEST_CASE("a reader with no peers describes nothing", "[serve][board][reader]") {
  auto [mock, ptr] = MakeMockHttpClient();
  int calls = 0;
  ptr->simulate_get = [&calls](const std::string&, const std::vector<std::string>&) {
    ++calls;
    return pu::http::Response{200, std::string("{}")};
  };

  PeerConfig none;
  none.self_name = "A";
  BoardReader reader(none, ptr);

  CHECK(reader.PullAll().empty());
  CHECK(reader.Describe({}).empty());
  CHECK(calls == 0);
}
