// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu::cli {

struct AppContext;

// Entry points for CLI commands
int RunAsk(int argc, char* argv[]);
int RunChat(int argc, char* argv[]);
int RunLearn(int argc, char* argv[]);

} // namespace pu::cli
