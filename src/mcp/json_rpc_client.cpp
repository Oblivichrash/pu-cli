// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/json_rpc_client.hpp"
#include <spdlog/spdlog.h>

namespace pu::mcp {

JsonRpcClient::JsonRpcClient(Transport& transport) : transport_(transport) {}

std::future<nlohmann::json> JsonRpcClient::SendRequest(
    const std::string& method,
    const nlohmann::json& params) {
    int id = next_id_++;
    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id] = std::move(promise);
    }

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method}
    };
    if (!params.is_null()) req["params"] = params;

    if (!transport_.WriteLine(req.dump())) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second.set_exception(std::make_exception_ptr(std::runtime_error("Write failed")));
            pending_.erase(it);
        }
    }
    return future;
}

void JsonRpcClient::OnMessage(const std::string& line) {
    try {
        auto j = nlohmann::json::parse(line);
        if (j.contains("id") && j["id"].is_number_integer()) {
            int id = j["id"];
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                if (j.contains("error")) {
                    it->second.set_exception(
                        std::make_exception_ptr(std::runtime_error(j["error"].dump()))
                    );
                } else {
                    it->second.set_value(j);
                }
                pending_.erase(it);
            }
        }
        // Ignore notifications (no id)
    } catch (const std::exception& e) {
        spdlog::warn("JSON-RPC parse error: {}", e.what());
    }
}

} // namespace pu::mcp
