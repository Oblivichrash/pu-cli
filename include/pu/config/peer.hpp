// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pu::config {

struct PeerEntry {
  std::string name;
  std::string url;
};

struct PeerConfig {
  std::string self_name;
  std::string token;
  std::vector<PeerEntry> peers;

  std::optional<PeerEntry> FindPeer(const std::string& name) const;
  bool Knows(const std::string& name) const;
  bool Accepts(const std::string& presented_token) const;
};

std::optional<PeerConfig> FindPeerConfig();
std::optional<PeerConfig> LoadPeerConfig(const std::string& config_path);

bool ConstantTimeEquals(const std::string& a, const std::string& b);

}  // namespace pu::config
