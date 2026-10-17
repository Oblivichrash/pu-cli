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
  return boost::json::object{{"peer", peer}, {"message", message}};
}

}  // namespace

TEST_CASE("send_to_peer posts the message to the named peer") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  ptr->simulate_response = [](const std::string&, const std::string&, const std::vector<std::string>&,
                              pu::http::WriteCallback write_cb) {
    const std::string body = R"({"success":true,"accepted":true})";
    write_cb(const_cast<char*>(body.data()), body.size());
  };

  tools::PeerTool tool(cfg, ptr);
  std::string reply;
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool());
  CHECK(ptr->last_url == "http://127.0.0.1:8901/api/peer/message");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "from", "") == "A");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "message", "") ==
        "hello");
  CHECK(std::find(ptr->last_headers.begin(), ptr->last_headers.end(),
                  "Authorization: Bearer shared-secret") != ptr->last_headers.end());
}

TEST_CASE("send_to_peer tells the model the answer will arrive later") {
  const auto cfg = OnePeer();
  auto [mock, ptr] = tests::MakeMockHttpClient();
  ptr->simulate_response = [](const std::string&, const std::string&, const std::vector<std::string>&,
                              pu::http::WriteCallback write_cb) {
    const std::string body = R"({"success":true,"accepted":true})";
    write_cb(const_cast<char*>(body.data()), body.size());
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("output").as_string().find("later turn") != std::string::npos);
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
  ptr->simulate_response = [](const std::string&, const std::string&, const std::vector<std::string>&,
                              pu::http::WriteCallback write_cb) {
    const std::string body = R"({"success":true,"accepted":true})";
    write_cb(const_cast<char*>(body.data()), body.size());
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;
  ctx.peer_origin = "B";

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "the answer is 4"), ctx));

  CHECK(j.as_object().at("success").as_bool());
  CHECK(ptr->last_url == "http://127.0.0.1:8901/api/peer/message");
  CHECK(json::ValueOrDefault<std::string>(boost::json::parse(ptr->last_body), "from", "") == "A");
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

TEST_CASE("send_to_peer requires both a peer and a message") {
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
  ptr->simulate_response = [](const std::string&, const std::string&, const std::vector<std::string>&,
                              pu::http::WriteCallback write_cb) {
    const std::string body = R"({"success":false,"error":"Invalid peer token"})";
    write_cb(const_cast<char*>(body.data()), body.size());
  };

  tools::PeerTool tool(cfg, ptr);
  pu::ToolContext ctx;

  const auto j = boost::json::parse(tool.Execute(SendArgs("B", "hello"), ctx));

  CHECK(j.as_object().at("success").as_bool() == false);
  CHECK(j.as_object().at("error").as_string().find("Invalid peer token") != std::string::npos);
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
