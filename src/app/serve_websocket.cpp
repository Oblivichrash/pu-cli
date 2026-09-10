// SPDX-License-Identifier: GPL-3.0-only
#include "serve_internal.hpp"

#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pu/core/json.hpp"
#include "pu/executor.hpp"
#include "pu/runtime.hpp"

namespace pu::cli::detail {

namespace beast = boost::beast;
namespace websocket = beast::websocket;

void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req,
                         Runtime& runtime, std::mutex& io_mutex,
                         std::shared_ptr<ActiveWebSocket> active_ws) {
  beast::error_code ec;

  // Replace any previously active client: abort its in-flight request first so
  // the old worker stops streaming into a socket we are about to close.
  if (active_ws->running) {
    active_ws->cancel_token->store(true);
    if (active_ws->worker_thread.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      active_ws->worker_thread.detach();
    }
    if (active_ws->ws && active_ws->ws->is_open()) {
      beast::error_code close_ec;
      active_ws->ws->close(websocket::close_code::normal, close_ec);
    }
    active_ws->running = false;
  }

  auto new_ws = std::make_unique<websocket::stream<tcp::socket>>(std::move(socket));
  new_ws->accept(req, ec);
  if (ec) {
    spdlog::warn("WebSocket accept error: {}", ec.message());
    return;
  }

  {
    std::lock_guard<std::mutex> lock(active_ws->mtx);
    active_ws->ws = std::move(new_ws);
    active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
    active_ws->running = true;
  }

  active_ws->worker_thread = std::thread([&runtime, &io_mutex, active_ws]() {
    while (active_ws->running) {
      beast::flat_buffer buffer;
      std::string text;
      beast::error_code ec;
      active_ws->ws->read(buffer, ec);
      if (ec) {
        if (ec != websocket::error::closed)
          spdlog::warn("WebSocket read error: {}", ec.message());
        active_ws->cancel_token->store(true);
        active_ws->running = false;
        break;
      }
      text = beast::buffers_to_string(buffer.data());

      boost::json::value jv;
      try {
        jv = boost::json::parse(text);
      } catch (...) {
        continue;
      }
      if (!jv.is_object())
        continue;

      std::string type = json::ValueOrDefault<std::string>(jv, "type", "");
      if (type == "cancel") {
        active_ws->cancel_token->store(true);
        continue;
      } else if (type == "run") {
        active_ws->cancel_token->store(true);
        active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
        auto token = active_ws->cancel_token;

        std::string payload_text = json::ValueOrDefault<std::string>(
            json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{}),
            "text", "");
        if (payload_text.empty())
          continue;

        std::thread worker([&runtime, &io_mutex, active_ws, token, payload_text]() {
          bool is_command = false;
          ExecutionResult result;

          // Serialize and send one JSON frame to the WebSocket client.
          auto send_frame = [&](const boost::json::value& ev) {
            std::string msg = boost::json::serialize(ev);
            beast::error_code ec;
            std::lock_guard<std::mutex> ws_lock(active_ws->mtx);
            if (active_ws->ws && active_ws->ws->is_open())
              active_ws->ws->write(net::buffer(msg), ec);
          };

          // Tool call lifecycle callbacks: forward start/end events so the
          // front-end can display tool invocations in real time.
          Executor::ToolCallbacks tool_cb;
          tool_cb.on_start = [&](const std::string& id,
                                 const std::string& name,
                                 const boost::json::value& args) {
            boost::json::value frame = {
                {"type", "tool_start"},
                {"payload", {{"id", id}, {"name", name}, {"args", args}}}};
            send_frame(frame);
          };
          tool_cb.on_end = [&](const std::string& id,
                               const std::string& output,
                               const std::string& error) {
            boost::json::value frame = {
                {"type", "tool_end"},
                {"payload", {{"id", id}, {"output", output}, {"error", error}}}};
            send_frame(frame);
          };

          {
            std::lock_guard<std::mutex> lock(io_mutex);
            result = runtime.ProcessInput(
                payload_text, is_command, token,
                [&](const std::string& chunk) {
                  if (chunk.empty())
                    return;
                  boost::json::value ev = {{"type", "chunk"}, {"payload", {{"text", chunk}}}};
                  send_frame(ev);
                },
                tool_cb);
          }

          boost::json::value final;
          if (result.has_error)
            final = {{"type", "error"}, {"payload", {{"text", result.error_message}}}};
          else
            final = {{"type", "done"}};
          send_frame(final);
        });
        worker.detach();
      }
    }
  });
  active_ws->worker_thread.detach();
}

}  // namespace pu::cli::detail
