// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu::platform {

int ExecuteCommand(const std::string& command, std::string& output);
void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
