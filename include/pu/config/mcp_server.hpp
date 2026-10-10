// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <string>
#include <vector>

namespace pu::config {

struct McpServerConfig {
  std::string name;
  std::string command;
  std::vector<std::string> args;
  std::string url;
  std::map<std::string, std::string> headers;
};

}  // namespace pu::config
