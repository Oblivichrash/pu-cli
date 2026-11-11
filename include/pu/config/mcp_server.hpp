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
  // How long one JSON-RPC request may go unanswered. A server that is slow to start still
  // answers within a few seconds; a server that has gone away never answers at all, and the only
  // way to tell the two apart is to give up. Five seconds is long enough for a busy local
  // process, but a server that is known to be slow -- and a test that wants to prove the timeout
  // fires without waiting the full budget -- both need to say so.
  int timeout_ms = 5000;
};

}  // namespace pu::config
