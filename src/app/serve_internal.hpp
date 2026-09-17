// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Internal declarations shared by the `pu serve` implementation files
// (serve.cpp, serve_http_routes.cpp, serve_websocket.cpp). This is not a
// public header.

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "pu/core/cancel_token.hpp"
#include "pu/runtime.hpp"

namespace pu::cli::detail {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Route a plain (non-upgrade) request to the static-file or REST handler.
void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res);

// The single active WebSocket session. A new client connection replaces the
// previous one, so the server always drives exactly one chat at a time.
struct ActiveWebSocket {
  std::unique_ptr<websocket::stream<tcp::socket>> ws;
  std::thread worker_thread;
  CancelToken cancel_token;
  std::atomic<bool> running{false};
  std::mutex mtx;
};

// Take over `socket`, which already carries a WebSocket upgrade request, and
// serve the chat protocol until the client disconnects or is replaced.
void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req,
                         Runtime& runtime, std::mutex& io_mutex,
                         std::shared_ptr<ActiveWebSocket> active_ws);

}  // namespace pu::cli::detail
