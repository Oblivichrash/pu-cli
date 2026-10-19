// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/peer.hpp"
#include "tests/mocks/test_helpers.hpp"
#include "pu/core/base.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace pu;
using namespace pu::tests;

namespace fs = std::filesystem;

namespace {

struct TempPeerFile {
  fs::path path;
  TempPeerFile() { path = UniqueTempPath("pu_test_peers").string() + ".json"; }
  ~TempPeerFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void Write(const std::string& content) {
    std::ofstream file(path);
    file << content;
  }
};

config::PeerConfig TwoPeers() {
  config::PeerConfig cfg;
  cfg.self_name = "A";
  cfg.token = "shared-secret";
  cfg.peers = {{"B", "http://127.0.0.1:8901"}};
  return cfg;
}

}  // namespace

TEST_CASE("ConstantTimeEquals accepts identical strings") {
  CHECK(config::ConstantTimeEquals("secret", "secret"));
  CHECK(config::ConstantTimeEquals("", ""));
}

TEST_CASE("ConstantTimeEquals rejects a differing string of the same length") {
  CHECK_FALSE(config::ConstantTimeEquals("secret", "secreT"));
}

TEST_CASE("ConstantTimeEquals rejects a prefix without reading past the shorter string") {
  CHECK_FALSE(config::ConstantTimeEquals("secret", "sec"));
}

TEST_CASE("ConstantTimeEquals rejects a longer string that starts with the expected one") {
  CHECK_FALSE(config::ConstantTimeEquals("sec", "secret"));
}

TEST_CASE("A peer config finds a declared peer by name") {
  const auto found = TwoPeers().FindPeer("B");
  REQUIRE(found.has_value());
  CHECK(found->url == "http://127.0.0.1:8901");
  CHECK(TwoPeers().Knows("B"));
}

TEST_CASE("A peer config does not know an undeclared name") {
  CHECK_FALSE(TwoPeers().FindPeer("C").has_value());
  CHECK_FALSE(TwoPeers().Knows("C"));
}

TEST_CASE("A peer config accepts only its own token") {
  const auto cfg = TwoPeers();
  CHECK(cfg.Accepts("shared-secret"));
  CHECK_FALSE(cfg.Accepts("shared-secre"));
  CHECK_FALSE(cfg.Accepts(""));
}

TEST_CASE("A peer config without a token accepts nothing") {
  config::PeerConfig cfg = TwoPeers();
  cfg.token.clear();
  CHECK_FALSE(cfg.Accepts(""));
  CHECK_FALSE(cfg.Accepts("shared-secret"));
}

TEST_CASE("Peer configuration is read from a file") {
  TempPeerFile file;
  file.Write(
      R"({"self":{"name":"A"},"token":"t0","peers":[{"name":"B","url":"http://127.0.0.1:8901"}]})");

  const auto cfg = config::LoadPeerConfig(file.path.string());
  REQUIRE(cfg.has_value());
  CHECK(cfg->self_name == "A");
  CHECK(cfg->token == "t0");
  REQUIRE(cfg->peers.size() == 1);
  CHECK(cfg->peers[0].name == "B");
}

TEST_CASE("A peer entry without a url is skipped rather than trusted") {
  TempPeerFile file;
  file.Write(
      R"({"token":"t0","peers":[{"name":"B"},{"name":"C","url":"http://127.0.0.1:8902"}]})");

  const auto cfg = config::LoadPeerConfig(file.path.string());
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->peers.size() == 1);
  CHECK(cfg->peers[0].name == "C");
}

TEST_CASE("A missing peer configuration file is reported as absent") {
  CHECK_FALSE(config::LoadPeerConfig("definitely_not_a_peer_file.json").has_value());
}

TEST_CASE("Malformed peer configuration JSON is reported as an error") {
  TempPeerFile file;
  file.Write("{not json");
  CHECK_THROWS_AS(config::LoadPeerConfig(file.path.string()), GenericError);
}

TEST_CASE("The peer token environment variable overrides the configured one") {
  TempPeerFile file;
  file.Write(R"({"token":"from-file","peers":[]})");

#ifdef _WIN32
  _putenv_s("PU_PEER_TOKEN", "from-env");
#else
  setenv("PU_PEER_TOKEN", "from-env", 1);
#endif
  const auto cfg = config::LoadPeerConfig(file.path.string());
#ifdef _WIN32
  _putenv_s("PU_PEER_TOKEN", "");
#else
  unsetenv("PU_PEER_TOKEN");
#endif

  REQUIRE(cfg.has_value());
  CHECK(cfg->token == "from-env");
}
