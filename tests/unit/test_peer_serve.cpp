// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/config/peer.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/peer_harness.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <boost/json.hpp>

#include <string>

using pu::tests::HttpResponse;
using pu::tests::kPeerToken;
using pu::tests::PeerServer;
using pu::tests::WritePeersFile;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// A turn that relays once. `function.arguments` is a JSON string on the OpenAI wire: the
// protocol appends what arrives and parses the result, so an object would arrive as nothing.
boost::json::object RelayAnswer(const std::string& peer, const std::string& message) {
  const std::string arguments =
      boost::json::serialize(boost::json::object{{"peer", peer}, {"message", message}});
  return boost::json::object{
      {"tool_calls", boost::json::array{boost::json::object{
                         {"index", 0},
                         {"id", "call-1"},
                         {"type", "function"},
                         {"function",
                          boost::json::object{{"name", "send_to_peer"}, {"arguments", arguments}}},
                     }}}};
}

boost::json::object TextAnswer(const std::string& text) {
  return boost::json::object{{"content", text}};
}

}  // namespace

// The exchange the feature exists for: a turn on A calls send_to_peer, which is a real request
// to B's /api/peer/message, and B answers in a turn of its own. Both ends are servers; nothing
// between them is a mock.
//
// A's backend relays on its first request and then answers in words, which is what a real model
// does once the tool result is in the conversation; a turn is not over at its tool call.
TEST_CASE("A relayed message arrives as a turn on the peer that received it") {
  PeerServer a("A", [](int call) {
    return call <= 1 ? PeerServer::Reply{RelayAnswer("B", "ping"), "tool_calls"}
                     : PeerServer::Reply{TextAnswer("relayed"), "stop"};
  });
  // B's first turn answers by sending the reply back to A, which is the one route a turn that
  // arrived from a peer may take; the second request is the follow-up after the tool returns.
  PeerServer b("B", [](int call) {
    return call <= 1 ? PeerServer::Reply{RelayAnswer("A", "pong"), "tool_calls"}
                     : PeerServer::Reply{TextAnswer("sent"), "stop"};
  });
  WritePeersFile(a.Home(), "A", a.Port(), "B", b.Port());
  WritePeersFile(b.Home(), "B", b.Port(), "A", a.Port());

  // Both listen for the length of one exchange, so they are stopped together rather than per
  // block.
  b.Start();
  a.Start();

  // A turn on A, which is what a page starts over the WebSocket once it attaches. The
  // send_to_peer call inside it is the hop under test; `hop` is what the sender counts.
  const HttpResponse accepted = a.Client().PostRaw(
      "/api/peer/message", R"({"from":"B","message":"start","hop":0})", kPeerToken);

  INFO(accepted.status << " " << accepted.body);
  CHECK(accepted.status == 202);

  // A's message reaches B, B's reply "pong" reaches A, and each side's follow-up after its tool
  // result is the turn the transcript shows.
  const bool b_received = b.WaitForTranscript("ping");
  const bool a_heard = a.WaitForTranscript("pong");
  CAPTURE(a.Transcript());
  CAPTURE(b.Transcript());

  CHECK(b_received);
  CHECK(a_heard);

  // The turn A received records who asked, so a page names the peer rather than "You".
  CHECK(a.TranscriptContains("user: start"));

  a.Stop();
  b.Stop();
}

// What the page is told about a turn that arrived from a peer: who asked, and that it is not
// the page's to take back. Both are read from the two surfaces a page uses.
TEST_CASE("A peer's turn is named by its peer and cannot be rewound") {
  PeerServer a("A", TextAnswer("ok"), "stop");
  PeerServer b("B", TextAnswer("ok"), "stop");
  WritePeersFile(a.Home(), "A", a.Port(), "B", b.Port());
  WritePeersFile(b.Home(), "B", b.Port(), "A", a.Port());

  b.Start();
  a.Start();

  REQUIRE(a.Client()
              .PostRaw("/api/peer/message", R"({"from":"B","message":"ping","hop":0})",
                       kPeerToken)
              .status == 202);
  REQUIRE(a.WaitForTranscript("ok"));

  const boost::json::value history = boost::json::parse(a.Client().Get("/api/history"));
  REQUIRE(history.is_array());
  REQUIRE(history.as_array().size() >= 2);
  CHECK(history.as_array()[0].at("role") == "user");
  CHECK(history.as_array()[0].at("content") == "ping");
  CHECK(history.as_array()[0].at("from_peer") == "B");

  const HttpResponse refused =
      a.Client().PostFull("/api/rewind", boost::json::object{{"turn", 1}});
  CHECK(refused.status == 400);
  CHECK(Contains(refused.body, "arrived from a peer"));

  a.Stop();
  b.Stop();
}

// A server with no peers.json is not part of a topology: the endpoint answers 404 and the tool
// is never registered, both following from the one absent file.
TEST_CASE("A server without peers.json does not mount the peer endpoint") {
  PeerServer lone("L", TextAnswer("ok"), "stop");
  lone.Start();

  const HttpResponse res = lone.Client().PostRaw(
      "/api/peer/message", R"({"from":"B","message":"hello","hop":0})", kPeerToken);

  CHECK(res.status == 404);
  CHECK(Contains(res.body, "not configured"));

  lone.Stop();
}

// The token is the only thing between a peer and a turn, and an undeclared sender is refused
// even with it. Both refusals happen before any turn is queued.
TEST_CASE("The peer endpoint refuses a wrong token and an undeclared sender") {
  PeerServer a("A", TextAnswer("ok"), "stop");
  PeerServer b("B", TextAnswer("ok"), "stop");
  WritePeersFile(a.Home(), "A", a.Port(), "B", b.Port());
  WritePeersFile(b.Home(), "B", b.Port(), "A", a.Port());

  b.Start();
  a.Start();

  // Both refusals are `RequestRefused`, which the HTTP layer answers with 400 rather than 401
  // or 403: the endpoint is the one authenticated surface, so a 401 would describe it more
  // precisely. The case asserts what it does today and the reason it gives.
  const HttpResponse wrong_token = a.Client().PostRaw(
      "/api/peer/message", R"({"from":"B","message":"hello","hop":0})", "not-the-token");
  CHECK(wrong_token.status == 400);
  CHECK(Contains(wrong_token.body, "Invalid peer token"));

  const HttpResponse undeclared = a.Client().PostRaw(
      "/api/peer/message", R"({"from":"stranger","message":"hi","hop":0})", kPeerToken);
  CHECK(undeclared.status == 400);
  CHECK(Contains(undeclared.body, "Unknown peer"));

  CHECK_FALSE(a.TranscriptContains("hello"));
  CHECK_FALSE(a.TranscriptContains("hi"));

  a.Stop();
  b.Stop();
}
