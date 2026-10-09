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

#include "pu/core/base.hpp"
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

// The chat being served, and the client watching it. One client at a time: a new
// connection replaces the previous one as the one being written to, which is what
// keeps the conversation single. A turn belongs to the client watching it — whoever
// leaves, or is replaced, ends it — so the server never drives a chat that nobody is
// looking at, and closing the page is as good a way to stop a reply as the Stop
// button. What such a turn wrote is half an answer and is not kept.
struct ActiveWebSocket {
  std::shared_ptr<websocket::stream<tcp::socket>> client;  // null while nobody listens
  CancelToken cancel_token;
  std::mutex mtx;  // guards client and cancel_token
};

// Take over `socket`, which already carries a WebSocket upgrade request, and
// serve the chat protocol until the client disconnects or is replaced.
void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req, Runtime& runtime,
                         std::mutex& io_mutex, std::shared_ptr<ActiveWebSocket> active_ws);

}  // namespace pu::cli::detail
