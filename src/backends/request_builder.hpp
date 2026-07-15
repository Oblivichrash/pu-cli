// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pu::backends {

using json = nlohmann::json;

enum class BackendFlavor { kOllama, kOpenAI };

class RequestBuilder {
public:
    static json BuildChatRequest(
        BackendFlavor flavor,
        const std::string& model,
        float temperature,
        const std::vector<pu::backend::Message>& history,
        const std::optional<std::string>& system_prompt
    );

    static json BuildChatRequestWithTools(
        BackendFlavor flavor,
        const std::string& model,
        float temperature,
        const std::vector<pu::backend::Message>& history,
        const std::optional<std::string>& system_prompt,
        const std::vector<pu::backend::ToolDefinition>& tools
    );

private:
    static json BuildMessagesJson(const std::vector<pu::backend::Message>& history);
    static json BuildMessagesJsonOllama(const std::vector<pu::backend::Message>& history);
    static json BuildToolsJsonOllama(const std::vector<pu::backend::ToolDefinition>& tools);
    static json BuildToolsJsonOpenAI(const std::vector<pu::backend::ToolDefinition>& tools);
    static std::string RoleToString(pu::backend::Message::Role role);
};

} // namespace pu::backends
