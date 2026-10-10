// SPDX-License-Identifier: GPL-3.0-only
#pragma once

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

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res);

struct ActiveWebSocket {
  std::shared_ptr<websocket::stream<tcp::socket>> client;  // null while nobody listens
  CancelToken cancel_token;
  std::mutex mtx;  // guards client and cancel_token
};

void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req, Runtime& runtime,
                         std::mutex& io_mutex, std::shared_ptr<ActiveWebSocket> active_ws);

}  // namespace pu::cli::detail
