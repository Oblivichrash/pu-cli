// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>

namespace pu {
void InitLogging(const std::string& log_level = "", bool trace_enabled = false);
void ShutdownLogging();
}