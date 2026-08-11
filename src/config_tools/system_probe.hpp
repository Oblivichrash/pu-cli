// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <nlohmann/json.hpp>

#include "pu/http_client.hpp"

namespace pu::config_tools {

// Returns os_name, kernel_version, arch, working_dir, provider_status.
nlohmann::json ProbeSystem();
nlohmann::json ProbeSystem(pu::http::HttpClient& http);

}  // namespace pu::config_tools
