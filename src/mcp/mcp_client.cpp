// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/mcp_client.hpp"
#include "pu/mcp/stdio_transport.hpp"
#include "pu/mcp/json_rpc_client.hpp"
#include <spdlog/spdlog.h>
#include <future>
#include <chrono>

namespace pu::mcp {

struct McpClient::Impl {
    McpServerConfig config;
    std::unique_ptr<StdioTransport> transport;
    std::unique_ptr<JsonRpcClient> rpc;
    bool connected = false;
    std::vector<ToolDefinition> cached_tools;
};

McpClient::McpClient(const McpServerConfig& config)
    : pimpl_(std::make_unique<Impl>()) {
    pimpl_->config = config;
}

McpClient::~McpClient() { Disconnect(); }

bool McpClient::Connect() {
    if (pimpl_->connected) return true;

    pimpl_->transport = std::make_unique<StdioTransport>(
        pimpl_->config.command, pimpl_->config.args
    );
    if (!pimpl_->transport->Start([this](const std::string& line) {
        if (pimpl_->rpc) pimpl_->rpc->OnMessage(line);
    })) {
        spdlog::error("MCP transport start failed for '{}'", pimpl_->config.name);
        return false;
    }

    pimpl_->rpc = std::make_unique<JsonRpcClient>(*pimpl_->transport);
    if (!Handshake()) {
        spdlog::error("MCP handshake failed for '{}'", pimpl_->config.name);
        Disconnect();
        return false;
    }

    pimpl_->connected = true;
    spdlog::info("MCP client connected: {}", pimpl_->config.name);
    return true;
}

void McpClient::Disconnect() {
    if (pimpl_->transport) {
        pimpl_->transport->Stop();
        pimpl_->transport.reset();
    }
    pimpl_->rpc.reset();
    pimpl_->connected = false;
    pimpl_->cached_tools.clear();
}

bool McpClient::Handshake() {
    nlohmann::json init_params = {
        {"protocolVersion", "2024-11-05"},
        {"clientInfo", {{"name", "pu-cli"}, {"version", "0.3.1"}}},
        {"capabilities", {{"tools", true}}}
    };
    try {
        auto resp = SendRequest("initialize", init_params);
        if (!resp.contains("result")) return false;
        // Send initialized notification (no response needed)
        pimpl_->transport->WriteLine(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"method", "initialized"}
        }.dump());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Handshake error: {}", e.what());
        return false;
    }
}

nlohmann::json McpClient::SendRequest(const std::string& method, const nlohmann::json& params, int timeout_ms) {
    if (!pimpl_->connected || !pimpl_->rpc) {
        throw std::runtime_error("MCP client not connected");
    }
    auto future = pimpl_->rpc->SendRequest(method, params);
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        throw std::runtime_error("MCP request timeout");
    }
    return future.get();
}

std::vector<ToolDefinition> McpClient::ListTools() {
    if (!pimpl_->connected) {
        spdlog::warn("Cannot list tools: client not connected");
        return {};
    }
    if (!pimpl_->cached_tools.empty()) return pimpl_->cached_tools;

    try {
        auto resp = SendRequest("tools/list", nullptr);
        if (resp.contains("result") && resp["result"].contains("tools")) {
            std::vector<ToolDefinition> defs;
            for (const auto& t : resp["result"]["tools"]) {
                ToolDefinition def;
                def.name = t.value("name", "");
                def.description = t.value("description", "");
                if (t.contains("inputSchema")) {
                    def.parameters_schema = t["inputSchema"].dump();
                } else {
                    def.parameters_schema = "{}";
                }
                defs.push_back(def);
            }
            pimpl_->cached_tools = defs;
            return defs;
        }
    } catch (const std::exception& e) {
        spdlog::error("ListTools failed: {}", e.what());
    }
    return {};
}

std::string McpClient::CallTool(const std::string& name, const nlohmann::json& arguments) {
    if (!pimpl_->connected) {
        return "Error: MCP client not connected";
    }
    try {
        nlohmann::json params = {{"name", name}, {"arguments", arguments}};
        auto resp = SendRequest("tools/call", params);
        if (resp.contains("result") && resp["result"].contains("content")) {
            std::string output;
            for (const auto& item : resp["result"]["content"]) {
                if (item.contains("type") && item["type"] == "text") {
                    output += item.value("text", "") + "\n";
                }
            }
            if (!output.empty()) return output;
            return resp["result"].dump();
        } else if (resp.contains("error")) {
            return "MCP error: " + resp["error"].dump();
        }
        return resp.dump();
    } catch (const std::exception& e) {
        spdlog::error("CallTool failed: {}", e.what());
        return std::string("MCP call error: ") + e.what();
    }
}

bool McpClient::IsConnected() const { return pimpl_->connected; }

} // namespace pu::mcp