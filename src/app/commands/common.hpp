// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent_config.hpp"
#include "pu/agent.hpp"
#include "pu/conversation.hpp"
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>

namespace pu::cli {

struct AppContext {
    config::AgentsConfig config;
    agent::AgentManager manager;
    std::string config_path;
};

AppContext SetupAppContext(const std::string& requested_agent, bool show_reasoning);

inline std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

inline std::string CurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

inline std::string GenerateId() {
    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::ostringstream ss;
    ss << std::hex << nanos;
    return "conv-" + ss.str();
}

void PrintAgents(const config::AgentsConfig& config, const std::string& current);
void PrintConversationList(const std::vector<Conversation>& convs);
void PrintChatHelp();

} // namespace pu::cli
