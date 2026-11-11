// SPDX-License-Identifier: GPL-3.0-only
// One probe run, in the layer that needs no session. `pu probe` and `pu-probe` both come
// through here, so a field name read by one is a field name read by the other.

#pragma once

#include <string>

#include "pu/config/backend.hpp"
#include "pu/core/http_client.hpp"
#include "pu/tools_cli/exit_code.hpp"

namespace pu::tools_cli {

struct ProbeOptions {
  std::string agent;
  std::string gateway;
  std::string model;
  std::string prompt;
  std::string out_dir;
  bool save = false;
  bool listing = false;
};

// Runs the probe and prints its report. The backend is already resolved: who chose it —
// a running session's config, or a standalone invocation's own config read — is not this
// function's concern, and keeping it that way is what lets the tool skip the Runtime.
ExitCode RunProbe(const ProbeOptions& options, const config::Backend& backend,
                    http::HttpClient& http);

}  // namespace pu::tools_cli
