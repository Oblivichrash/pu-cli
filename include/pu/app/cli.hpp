// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/program_options.hpp>

#include <cstdint>
#include <string>

namespace pu {

class Runtime;

namespace cli {

// The whole command line, assembled in one place so that a name registered twice
// is caught where a test can drive it rather than at the process boundary.
struct CommandLine {
  boost::program_options::options_description all;
  boost::program_options::positional_options_description positional;
};

CommandLine BuildCommandLine();

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime);
int RunChat(const std::string& agent, Runtime& runtime);
int RunServe(const std::string& host, std::uint16_t port, Runtime& runtime);

}  // namespace cli
}  // namespace pu
