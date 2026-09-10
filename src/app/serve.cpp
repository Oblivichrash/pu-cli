// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "serve_internal.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pu/core/platform.hpp"
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
using detail::RunWebSocketSession;

}  // namespace

int RunServe(const std::string& host, int port, Runtime& runtime) {
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

  std::function<void(beast::error_code, tcp::socket)> do_accept =
      [&](beast::error_code ec, tcp::socket socket) {
    if (ec) {
      if (ec != net::error::operation_aborted)
        spdlog::warn("Accept error: {}", ec.message());
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
        if (ec)
          spdlog::warn("HTTP write error: {}", ec.message());
      }
    }).detach();

    acceptor.async_accept(ioc, do_accept);
  };

  acceptor.async_accept(ioc, do_accept);

  std::thread ioc_thread([&]() { ioc.run(); });

  while (!pu::platform::IsInterrupted())
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ioc.stop();
  if (ioc_thread.joinable())
    ioc_thread.join();

  if (active_ws->running) {
    active_ws->cancel_token->store(true);
    if (active_ws->worker_thread.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      active_ws->worker_thread.detach();
    }
    if (active_ws->ws && active_ws->ws->is_open()) {
      beast::error_code ec;
      active_ws->ws->close(websocket::close_code::normal, ec);
    }
  }

  runtime.Shutdown();
  return 0;
}

}  // namespace pu::cli
