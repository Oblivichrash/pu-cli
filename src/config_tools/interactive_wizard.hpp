// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

#include "pu/agent_config.hpp"

namespace pu::config_tools {

// Runs the interactive agent creation wizard, returning the new AgentEntry.
config::AgentEntry RunInteractiveWizard();

}  // namespace pu::config_tools