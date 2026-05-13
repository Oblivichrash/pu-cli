// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu::platform {

int ExecuteCommand(const std::string& command, std::string& output);
bool IsDangerous(const std::string& command, std::string* reason = nullptr);
void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
