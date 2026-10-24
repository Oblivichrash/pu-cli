// SPDX-License-Identifier: GPL-3.0-only

#include "pu/tools/peer_tool.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/session/message.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace pu;

namespace {

config::PeerConfig OnePeer() {
  config::PeerConfig cfg;
  cfg.self_name = "A";
  cfg.token = "shared-secret";
  cfg.peers = {{"B", "http://127.0.0.1:8901"}};
  return cfg;
}

boost::json::value SendArgs(const std::string& peer, const std::string& message) {
  return boost::json::object{{"peer", peer}, {"intent", "look at it"}, {"message", message}};
}

// A board that took the job, and a server that queued the turn under it.
void AnswerBothCalls(const std::string& url, pu::http::WriteCallback write_cb) {
  const std::string posted = R"({"job":{"id":"j1","state":"open","due_at":"2026-01-01T00:00:00Z"}})";
  const std::string accepted = R"({"success":true,"accepted":true,"turn":"t1","model":"m"})";
  const std::string body = url.find("/api/jobs") != std::string::npos ? posted : accepted;
  write_cb(const_cast<char*>(body.data()), body.size());
}

}  // namespace

TEST_CASE("send_to_peer posts the job before it says anything") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  std::vector<std::string> seen;
  ptr->simulate_response = [&](const std::string& url, const std::string&,
                               const std::vector<std::string>&,
                               pu::http::WriteCallback write_cb) {
    seen.push_back(url);
    AnswerBothCalls(url, write_cb);
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool());
  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == "http://127.0.0.1:8901/api/jobs");
  CHECK(seen[1] == "http://127.0.0.1:8901/api/peer/message");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "from", "") == "A");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "job", "") == "j1");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "message", "") ==
        "hello");
  REQUIRE(ptr->last_headers.size() == 2);
  CHECK(std::string(ptr->last_headers[0]) == "Content-Type: application/json");
  CHECK(std::string(ptr->last_headers[1]) == "Authorization: Bearer shared-secret");
}

TEST_CASE("send_to_peer posts the intent and the deadline it was given") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  std::string posted;
  ptr->simulate_response = [&](const std::string& url, const std::string& body,
                               const std::vector<std::string>&,
                               pu::http::WriteCallback write_cb) {
    if (url.find("/api/jobs") != std::string::npos) posted = body;
    AnswerBothCalls(url, write_cb);
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  auto args = SendArgs("B", "hello");
  args.as_object()["due"] = "2h";
  const auto j = boost::json::parse(tool.Execute(args, ctx));

  CHECK(j.as_object().at("success").as_bool());
  const auto request = boost::json::parse(posted);
  CHECK(json::ValueOrDefault<std::string>(request, "to", "") == "B");
  CHECK(json::ValueOrDefault<std::string>(request, "intent", "") == "look at it");
  CHECK(json::ValueOrDefault<std::string>(request, "due", "") == "2h");
}

TEST_CASE("send_to_peer tells the model the answer will arrive later") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  ptr->simulate_response = [](const std::string& url, const std::string&,
                              const std::vector<std::string>&, pu::http::WriteCallback write_cb) {
    AnswerBothCalls(url, write_cb);
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  const std::string said = j.as_object().at("output").as_string().c_str();
  CHECK(said.find("later turn") != std::string::npos);
  CHECK(said.find("j1") != std::string::npos);
  CHECK(said.find("2026-01-01T00:00:00Z") != std::string::npos);
  CHECK(j.as_object().at("blocked").as_bool() == false);
}

TEST_CASE("send_to_peer refuses to relay a peer turn onward") {
  config::PeerConfig cfg = OnePeer();
  cfg.peers.push_back({"C", "http://127.0.0.1:8902"});
  auto [mock, ptr] = tests::MakeMockHttpClient();

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;
  ctx.peer_origin = "B";

  const auto j = boost::json::parse(tool.Execute(SendArgs("C", "pass it on"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("blocked").as_bool());
  CHECK(j.as_object().at("error").as_string().find("B") != std::string::npos);
  CHECK(j.as_object().at("error").as_string().find(session::kPeerOriginNote) != std::string::npos);
  CHECK(ptr->last_url.empty());
}

TEST_CASE("send_to_peer lets a peer turn answer the agent that asked") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();

  ptr->simulate_response = [](const std::string& url, const std::string&,
                              const std::vector<std::string>&, pu::http::WriteCallback write_cb) {
    AnswerBothCalls(url, write_cb);
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;
  ctx.peer_origin = "B";

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "the answer is 4"), ctx));

  CHECK(j.as_object().at("success").as_bool());
  CHECK(ptr->last_url == "http://127.0.0.1:8901/api/peer/message");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "from", "") == "A");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "job", "") == "j1");
}

TEST_CASE("a board that refuses the job stops the call before anything is said") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  std::vector<std::string> called;
  ptr->simulate_response = [&](const std::string& url, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback) {
    called.push_back(url);
    throw std::runtime_error("HTTP error 400: the board already holds 32 unfinished jobs");
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("blocked").as_bool() == false);
  CHECK(j.as_object().at("error").as_string().find("32 unfinished jobs") != std::string::npos);
  REQUIRE(called.size() == 1);
  CHECK(called.at(0) == "http://127.0.0.1:8901/api/jobs");
}

TEST_CASE("send_to_peer reports an unknown peer without calling out") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("C", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("error").as_string().find("C") != std::string::npos);
  CHECK(ptr->last_url.empty());
}

TEST_CASE("send_to_peer requires a peer, an intent and a message") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", ""), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(ptr->last_url.empty());
}

TEST_CASE("send_to_peer reports a peer that refuses the message") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  ptr->simulate_response = [](const std::string& url, const std::string&,
                              const std::vector<std::string>&, pu::http::WriteCallback write_cb) {
    if (url.find("/api/jobs") != std::string::npos) {
      AnswerBothCalls(url, write_cb);
      return;
    }
    const std::string body = R"({"success":false,"error":"Invalid peer token"})";
    write_cb(const_cast<char*>(body.data()), body.size());
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("error").as_string().find("Invalid peer token") != std::string::npos);
  CHECK(j.as_object().at("error").as_string().find("j1") != std::string::npos);
}

TEST_CASE("send_to_peer reports a peer that cannot be reached") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  ptr->simulate_response = [](const std::string&, const std::string&, const std::vector<std::string>&,
                              pu::http::WriteCallback) {
    throw pu::HttpError("connection refused");
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("error").as_string().find("connection refused") != std::string::npos);
}

TEST_CASE("send_to_peer offers only the configured peers") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();

  tools::PeerTool tool(cfg, ptr);
  const auto schema = tool.ParametersSchema();
  const auto& names = schema.as_object().at("properties").as_object().at("peer").as_object().at("enum");

  REQUIRE(names.as_array().size() == 1);
  CHECK(names.as_array()[0].as_string() == "B");
}
