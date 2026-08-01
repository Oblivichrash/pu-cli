// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu {

class Runtime;

namespace cli {

int RunAsk(int argc, char* argv[], Runtime& runtime);
int RunChat(int argc, char* argv[], Runtime& runtime);

}  // namespace pu::cli
}  // namespace pu