// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include "serve_internal.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "pu/core/json.hpp"
#include "pu/core/platform.hpp"
#include "pu/executor.hpp"
#include "pu/runtime.hpp"

namespace pu::cli {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using detail::ActiveWebSocket;
using detail::DispatchHttpRequest;

void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req, Runtime& runtime,
                         std::mutex& io_mutex, std::shared_ptr<ActiveWebSocket> active_ws) {
  auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));

  beast::error_code ec;
  ws->accept(req, ec);
  if (ec) {
    spdlog::warn("WebSocket accept error: {}", ec.message());
    return;
  }

  {
    std::lock_guard<std::mutex> lock(active_ws->mtx);
    if (active_ws->client) {
      boost::json::value busy = {
          {"type", "busy"},
          {"payload", {{"text", "This session is already open in another page."}}}};
      const std::string message = boost::json::serialize(busy);
      beast::error_code write_ec;
      ws->write(net::buffer(message), write_ec);
      beast::error_code close_ec;
      ws->close(websocket::close_code::policy_error, close_ec);
      return;
    }
    active_ws->client = ws;
  }

  for (;;) {
    beast::flat_buffer buffer;
    beast::error_code read_ec;
    ws->read(buffer, read_ec);
    if (read_ec) {
      if (read_ec != websocket::error::closed) {
        spdlog::warn("WebSocket read error: {}", read_ec.message());
      }
      break;
    }

    const std::string text = beast::buffers_to_string(buffer.data());

    boost::json::value jv;
    try {
      jv = boost::json::parse(text);
    } catch (const boost::system::system_error& e) {
      boost::json::value error = {
          {"type", "error"}, {"payload", {{"text", std::string("Invalid JSON: ") + e.what()}}}};
      const std::string message = boost::json::serialize(error);
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      if (active_ws->client == ws && ws->is_open()) {
        beast::error_code write_ec;
        ws->write(net::buffer(message), write_ec);
      }
      continue;
    }
    if (!jv.is_object()) continue;

    const std::string type = json::ValueOrDefault<std::string>(jv, "type", "");
    if (type == "cancel") {
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      active_ws->cancel_token->store(true);
      continue;
    }
    if (type == "note") {
      const std::string note = json::ValueOrDefault<std::string>(
          json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{}),
          "text", "");
      if (!note.empty()) runtime.SetUserNote(note);
      continue;
    }
    if (type == "stop_command") {
      const auto payload = json::ValueOrDefault<boost::json::value>(
          jv, "payload", boost::json::object{});
      const std::string note =
          json::ValueOrDefault<std::string>(payload, "text", "");
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      if (!active_ws->command_stop) {
        active_ws->command_stop = std::make_shared<std::atomic<bool>>(false);
      }
      active_ws->command_note = note;
      active_ws->command_stop->store(true);
      continue;
    }
    if (type != "run") continue;

    const std::string payload_text = json::ValueOrDefault<std::string>(
        json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{}), "text", "");
    if (payload_text.empty()) continue;

    CancelToken token;
    {
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      active_ws->cancel_token->store(true);
      active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
      token = active_ws->cancel_token;
    }

    std::thread([&runtime, &io_mutex, active_ws, token, payload_text]() {
      bool is_command = false;
      ExecutionResult result;

      const auto say = [&](const boost::json::value& frame) {
        std::lock_guard<std::mutex> lock(active_ws->mtx);
        if (active_ws->cancel_token != token) return;
        if (!active_ws->client || !active_ws->client->is_open()) return;
        const std::string message = boost::json::serialize(frame);
        beast::error_code write_ec;
        active_ws->client->write(net::buffer(message), write_ec);
      };

      ToolCallbacks tool_cb;
      tool_cb.on_start = [&](const std::string& id, const std::string& name,
                             const boost::json::value& args) {
        boost::json::value frame = {{"type", "tool_start"},
                                    {"payload", {{"id", id}, {"name", name}, {"args", args}}}};
        say(frame);
      };
      tool_cb.on_end = [&](const std::string& id, const std::string& output,
                           const std::string& error) {
        boost::json::value frame = {
            {"type", "tool_end"}, {"payload", {{"id", id}, {"output", output}, {"error", error}}}};
        say(frame);
      };

      CancelToken command_stop;
      {
        std::lock_guard<std::mutex> lock(active_ws->mtx);
        active_ws->command_stop = std::make_shared<std::atomic<bool>>(false);
        active_ws->command_note.clear();
        command_stop = active_ws->command_stop;
      }

      {
        std::lock_guard<std::mutex> lock(io_mutex);
        result = runtime.ProcessInput(
            payload_text, is_command, token,
            [&](std::string_view chunk) {
              if (chunk.empty()) return;
              boost::json::value frame = {{"type", "chunk"}, {"payload", {{"text", chunk}}}};
              say(frame);
            },
            tool_cb,
            [&](std::string_view thought) {
              if (thought.empty()) return;
              boost::json::value frame = {{"type", "thinking"}, {"payload", {{"text", thought}}}};
              say(frame);
            },
            command_stop);
      }

      if (!result.notice.empty()) {
        boost::json::value frame = {{"type", "notice"}, {"payload", {{"text", result.notice}}}};
        say(frame);
      }

      if (result.usage) {
        boost::json::value frame = {
            {"type", "usage"},
            {"payload",
             {{"prompt_tokens", result.usage->prompt_tokens},
              {"completion_tokens", result.usage->completion_tokens}}}};
        say(frame);
      }

      boost::json::value final;
      if (result.has_error) {
        final = {{"type", "error"}, {"payload", {{"text", result.error_message}}}};
      } else if (result.model.empty()) {
        final = {{"type", "done"}};
      } else {
        final = {{"type", "done"}, {"payload", {{"model", result.model}}}};
      }
      say(final);
    }).detach();
  }

  std::lock_guard<std::mutex> lock(active_ws->mtx);
  if (active_ws->client == ws) {
    active_ws->client = nullptr;
    active_ws->cancel_token->store(true);
  }
}

}  // namespace

int RunServe(const std::string& host, std::uint16_t port, Runtime& runtime) {
  try {
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  spdlog::info("pu serve listening on http://{}:{}", host, port);
  spdlog::info("WebSocket endpoint: ws://{}:{}/ws", host, port);
  spdlog::info("Workspace: {}", runtime.GetWorkspaceRoot().string());

  net::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(net::ip::make_address(host), port));

  std::mutex io_mutex;
  auto active_ws = std::make_shared<ActiveWebSocket>();
  active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);

  std::function<void(beast::error_code, tcp::socket)> do_accept = [&](beast::error_code ec,
                                                                      tcp::socket socket) {
    if (ec) {
      if (ec != net::error::operation_aborted) spdlog::warn("Accept error: {}", ec.message());
      return;
    }

    std::thread([&, socket = std::move(socket)]() mutable {
      beast::flat_buffer buffer;
      http::request<http::string_body> req;
      beast::error_code ec;
      http::read(socket, buffer, req, ec);
      if (ec) {
        spdlog::warn("HTTP read error: {}", ec.message());
        return;
      }

      if (websocket::is_upgrade(req)) {
        RunWebSocketSession(std::move(socket), std::move(req), runtime, io_mutex, active_ws);
      } else {
        http::response<http::string_body> res;
        DispatchHttpRequest(runtime, io_mutex, std::move(req), res);
        http::write(socket, res, ec);
        if (ec) spdlog::warn("HTTP write error: {}", ec.message());
      }
    }).detach();

    acceptor.async_accept(ioc, do_accept);
  };

  acceptor.async_accept(ioc, do_accept);

  std::thread ioc_thread([&]() { ioc.run(); });

  while (!pu::platform::IsInterrupted())
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ioc.stop();
  if (ioc_thread.joinable()) ioc_thread.join();

  std::shared_ptr<websocket::stream<tcp::socket>> client;
  {
    std::lock_guard<std::mutex> lock(active_ws->mtx);
    active_ws->cancel_token->store(true);
    client = std::move(active_ws->client);
  }
  if (client) {
    beast::error_code client_ec;
    beast::get_lowest_layer(*client).close(client_ec);
  }

  runtime.Shutdown();
  return 0;
}

}  // namespace pu::cli
