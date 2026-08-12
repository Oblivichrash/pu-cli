// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu {

class Runtime;

namespace cli {

int RunChat(int argc, char* argv[], Runtime& runtime);

}  // namespace pu::cli
}  // namespace pu

// `pu config` entry point; kept independent of Runtime.
int RunConfig(int argc, char* argv[]);