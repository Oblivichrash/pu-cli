// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/peer.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pu::config {

namespace {

constexpr const char* kDefaultPeerFileName = "peers.json";

std::vector<PeerEntry> ParsePeers(const pu::json::value& j) {
  std::vector<PeerEntry> peers;
  if (!pu::json::HasKey(j, "peers") || !j.at("peers").is_array()) return peers;

  for (const auto& item : j.at("peers").as_array()) {
    if (!item.is_object()) continue;
    PeerEntry entry;
    if (pu::json::HasKey(item, "name") && item.at("name").is_string())
      entry.name = boost::json::value_to<std::string>(item.at("name"));
    if (pu::json::HasKey(item, "url") && item.at("url").is_string())
      entry.url = boost::json::value_to<std::string>(item.at("url"));
    if (entry.name.empty() || entry.url.empty()) {
      spdlog::warn("Skipping a peer entry without a name and a url");
      continue;
    }
    peers.push_back(std::move(entry));
  }
  return peers;
}

const char* PeerTokenFromEnv() {
  const char* token = std::getenv("PU_PEER_TOKEN");
  return token != nullptr && token[0] != '\0' ? token : nullptr;
}

}  // namespace

std::optional<PeerEntry> PeerConfig::FindPeer(const std::string& name) const {
  const auto it = std::find_if(peers.begin(), peers.end(),
                               [&](const PeerEntry& entry) { return entry.name == name; });
  if (it == peers.end()) return std::nullopt;
  return *it;
}

bool PeerConfig::Knows(const std::string& name) const { return FindPeer(name).has_value(); }

bool PeerConfig::Accepts(const std::string& presented_token) const {
  return !token.empty() && ConstantTimeEquals(token, presented_token);
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
  const std::size_t length = std::max(a.size(), b.size());
  unsigned char difference = static_cast<unsigned char>(a.size() ^ b.size());
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned char left = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char right = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    difference = static_cast<unsigned char>(difference | (left ^ right));
  }
  return difference == 0;
}

std::optional<PeerConfig> LoadPeerConfig(const std::string& config_path) {
  std::ifstream file(config_path);
  if (!file.is_open()) return std::nullopt;

  pu::json::value j;
  try {
    std::ostringstream buffer;
    buffer << file.rdbuf();
    j = pu::json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    throw pu::GenericError("Failed to parse peer configuration JSON: " + config_path);
  }

  PeerConfig result;
  if (pu::json::HasKey(j, "self") && j.at("self").is_object()) {
    const auto& self = j.at("self");
    if (pu::json::HasKey(self, "name") && self.at("name").is_string())
      result.self_name = boost::json::value_to<std::string>(self.at("name"));
  }
  if (pu::json::HasKey(j, "token") && j.at("token").is_string())
    result.token = boost::json::value_to<std::string>(j.at("token"));
  result.peers = ParsePeers(j);

  if (const char* from_env = PeerTokenFromEnv(); from_env != nullptr) result.token = from_env;

  return result;
}

std::optional<PeerConfig> FindPeerConfig() {
  const std::filesystem::path project = std::filesystem::path("./.pu") / kDefaultPeerFileName;
  if (std::filesystem::exists(project)) return LoadPeerConfig(project.string());

  const char* home = std::getenv("HOME");
  if (home != nullptr) {
    const std::filesystem::path user =
        std::filesystem::path(home) / ".pu" / kDefaultPeerFileName;
    if (std::filesystem::exists(user)) return LoadPeerConfig(user.string());
  }

  return std::nullopt;
}

}  // namespace pu::config
