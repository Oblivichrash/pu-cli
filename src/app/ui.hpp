// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/conversation.hpp"

namespace pu::cli {

std::string Trim(const std::string& s);
std::string CurrentTimestamp();
std::string GenerateId();
void PrintAgents(const pu::config::AgentsConfig& cfg, const std::string& current);
void PrintConversationList(const std::vector<pu::Conversation>& convs);
void PrintChatHelp();

}  // namespace pu::cli
