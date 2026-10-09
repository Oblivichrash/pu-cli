// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Internal declarations shared by the `pu serve` implementation files
// (serve.cpp, serve_http_routes.cpp, serve_websocket.cpp). This is not a
// public header.

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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

// The chat being served, and the client watching it. One chat at a time: a new
// client connection replaces the previous one as the one being written to, which is
// what keeps the conversation single. The turn, though, belongs to the chat and not
// to the socket — a page that reloads, or a connection that drops, leaves the
// request running, because a reload is how a reader comes back to a reply rather
// than a way to ask for it to be thrown away. `transcript` is what makes that
// possible: the frames of the turn in flight, kept so a client that attaches
// halfway through can be shown the answer from its beginning.
struct ActiveWebSocket {
  std::shared_ptr<websocket::stream<tcp::socket>> client;  // null while nobody listens
  CancelToken cancel_token;
  boost::json::array transcript;
  bool turn_in_flight{false};
  std::mutex mtx;  // guards client, cancel_token, transcript, turn_in_flight
};

// Take over `socket`, which already carries a WebSocket upgrade request, and
// serve the chat protocol until the client disconnects or is replaced.
void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req, Runtime& runtime,
                         std::mutex& io_mutex, std::shared_ptr<ActiveWebSocket> active_ws);

}  // namespace pu::cli::detail
