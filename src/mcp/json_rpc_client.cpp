// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/json_rpc_client.hpp"
#include "pu/json.hpp"
#include <spdlog/spdlog.h>

namespace pu::mcp {

JsonRpcClient::JsonRpcClient(Transport& transport) : transport_(transport) {}

std::future<boost::json::value> JsonRpcClient::SendRequest(
    const std::string& method,
    const boost::json::value& params) {
    int id = next_id_++;
    std::promise<boost::json::value> promise;
    auto future = promise.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id] = std::move(promise);
    }

    boost::json::value req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method}
    };
    if (!params.is_null()) req.as_object()["params"] = params;

    if (!transport_.WriteLine(boost::json::serialize(req))) {
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
        auto j = boost::json::parse(line);
        if (json::HasKey(j, "id") && j.at("id").is_int64()) {
            int id = boost::json::value_to<int>(j.at("id"));
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                if (json::HasKey(j, "error")) {
                    it->second.set_exception(
                        std::make_exception_ptr(std::runtime_error(boost::json::serialize(j.at("error"))))
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
