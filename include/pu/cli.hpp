// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu {

class Runtime;

namespace cli {

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime);
int RunChat(const std::string& agent, Runtime& runtime);
int RunServe(const std::string& host, int port, Runtime& runtime);

}  // namespace pu::cli
}  // namespace pu
