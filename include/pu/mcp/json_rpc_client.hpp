// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <nlohmann/json.hpp>
#include <future>
#include <unordered_map>
#include <mutex>
#include "pu/mcp/stdio_transport.hpp"

namespace pu::mcp {

class JsonRpcClient {
public:
    explicit JsonRpcClient(StdioTransport& transport);
    ~JsonRpcClient() = default;

    // Send a request, returning a future for the async response
    std::future<nlohmann::json> SendRequest(
        const std::string& method,
        const nlohmann::json& params = nullptr
    );

    // Handle incoming messages (called by transport callback)
    void OnMessage(const std::string& line);

private:
    StdioTransport& transport_;
    int next_id_ = 1;
    std::unordered_map<int, std::promise<nlohmann::json>> pending_;
    std::mutex mutex_;
};

} // namespace pu::mcp