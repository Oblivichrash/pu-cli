// SPDX-License-Identifier: GPL-3.0-only

//
// `pu serve`: the listener, the accept loop, and what each accepted request is handed to.
//
// What a connection does with a frame lives in serve_websocket.cpp, and what a turn does with a
// job in serve_runner.cpp. What is left here is neither: opening the socket, claiming the port,
// and sending each request to the WebSocket upgrade or to the REST dispatcher.
//

#include "pu/app/cli.hpp"

#include "serve_internal.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "pu/config/serve_registry.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/serve/board.hpp"
#include "pu/serve/peer_read.hpp"

namespace pu::cli {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using detail::DispatchHttpRequest;
using detail::PeerAddress;
using detail::SessionRunner;
using detail::WebSocketConnection;

// The address the banner may print. A listener bound to every interface answers on whatever
// address the machine has, which this side cannot know, so the bind itself is not a destination:
// printing `http://0.0.0.0:9000` hands the reader a URL that reaches their own loopback.
bool NamesADestination(const std::string& bind_address) {
  return !bind_address.empty() && bind_address != "0.0.0.0" && bind_address != "::" &&
         bind_address != "[::]";
}

}  // namespace

int RunServe(const std::string& bind_address, std::uint16_t port, Runtime& runtime) {
  try {
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  net::io_context ioc;
  std::uint16_t bound_port = port;
  const tcp::endpoint listen_endpoint(net::ip::make_address(bind_address), port);
  tcp::acceptor acceptor(ioc);
  {
    beast::error_code open_ec;
    acceptor.open(listen_endpoint.protocol(), open_ec);
    if (open_ec) {
      spdlog::error("Cannot open {}:{}: {}", bind_address, port, open_ec.message());
      return 1;
    }
    acceptor.set_option(net::socket_base::reuse_address(true), open_ec);
    if (open_ec) spdlog::warn("Cannot set reuse_address: {}", open_ec.message());
    acceptor.bind(listen_endpoint, open_ec);
    if (open_ec) {
      spdlog::error("Cannot bind {}:{}: {}", bind_address, port, open_ec.message());
      return 1;
    }
    acceptor.listen(net::socket_base::max_listen_connections, open_ec);
    if (open_ec) {
      spdlog::error("Cannot listen on {}:{}: {}", bind_address, port, open_ec.message());
      return 1;
    }
    bound_port = acceptor.local_endpoint(open_ec).port();
    if (open_ec) bound_port = port;
  }

  // Printed once the port is known, because a port of `0` is a request for any of them and the
  // number the reader needs is the one the kernel chose.
  if (NamesADestination(bind_address)) {
    spdlog::info("pu serve listening on http://{}:{}", bind_address, bound_port);
    spdlog::info("WebSocket endpoint: ws://{}:{}/ws", bind_address, bound_port);
  } else {
    spdlog::info("pu serve listening on port {} on every interface", bound_port);
    spdlog::info("Open http://<this machine's address>:{}/ in a browser", bound_port);
    spdlog::info("The page names this server by the address you reached it on, not by {}",
                 bind_address);
  }
  spdlog::info("Workspace: {}", runtime.WorkspaceRoot().string());

  std::mutex io_mutex;
  serve::Board board;
  // The registry entry is written after the port is known and before the first request is
  // accepted: a server a page can reach is a server the page can list, and a server that
  // never got its port never claims one. The page reads the directory from here too, so the
  // list names a workspace rather than a number to remember.
  config::ServeRecord serve_record;
  serve_record.directory = runtime.WorkspaceRoot().string();
  serve_record.bind = bind_address;
  serve_record.port = bound_port;
  serve_record.pid = platform::CurrentProcessId();
  serve_record.started = platform::ProcessStartToken(serve_record.pid);
  config::RegisterServeRecord(serve_record);
  auto runner = std::make_shared<SessionRunner>(runtime);  // The runner closes the job a peer turn answers, so it needs the board the routes serve.
  // It is handed over after both exist rather than through the constructor, which would
  // otherwise pin the declaration order of two independent locals.
  runner->AttachBoard(&board);
  // The digest is the peers' own boards, pulled once per round. The pull is also what tells a
  // peer this server is still working: it carries the token, and it is made on the thread that
  // runs the turn, so a turn that stops being worked stops reading.
  auto reader = std::make_shared<serve::BoardReader>(runtime.Peers().value_or(config::PeerConfig{}),
                                                     runtime.SharedHttp().get());
  runtime.SetContextDigest([&board, &runtime, reader] {
    const auto& peers = runtime.Peers();
    if (!peers.has_value()) return std::string();
    const std::string own = board.Digest(peers->self_name);
    const std::string theirs = reader->Describe(reader->PullAll());
    if (own.empty()) return theirs;
    if (theirs.empty()) return own;
    return own + "\n" + theirs;
  });

  std::function<void(beast::error_code, tcp::socket)> do_accept = [&](beast::error_code ec,
                                                                      tcp::socket socket) {
    if (ec) {
      if (ec != net::error::operation_aborted) spdlog::warn("Accept error: {}", ec.message());
      return;
    }

    auto strand = net::make_strand(ioc);
    const std::string peer = PeerAddress(socket, ec);
    auto conn = std::make_shared<WebSocketConnection>(std::move(socket), strand, runtime, *runner);
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto req = std::make_shared<http::request<http::string_body>>();
    // The request line and headers are read asynchronously: a socket parked in a blocking
    // read would hold the one thread the whole server runs on, and a browser's idle
    // preconnect would stall every other page until it gave up.
    auto header_timer =
        std::make_shared<net::steady_timer>(strand, std::chrono::seconds(30));
    header_timer->async_wait(net::bind_executor(strand, [conn, header_timer](beast::error_code ec) {
      if (ec) return;  // cancelled once the headers arrived
      spdlog::warn("HTTP header timed out");
      conn->Close();
    }));
    // `strand` is captured by value: the accept handler has returned by the time this runs,
    // and its locals are gone.
    http::async_read(
        conn->mutable_socket(), *buffer, *req,
        net::bind_executor(strand, [&runtime, &io_mutex, runner, &board, strand, conn, buffer,
                                    req, header_timer, peer, bound_port](beast::error_code ec,
                                                                         std::size_t) {
          header_timer->cancel();
          if (ec) {
            spdlog::warn("HTTP read error: {}", ec.message());
            conn->Close();
            return;
          }
          // The answer is built on this thread, so a request that cannot be answered still
          // ends the connection rather than the process.
          try {
            if (websocket::is_upgrade(*req)) {
              conn->Start(std::move(*req));
              return;
            }
            auto res = std::make_shared<http::response<http::string_body>>();
            DispatchHttpRequest(runtime, io_mutex, runner.get(), &board, std::move(*req), *res,
                                peer, bound_port);
            http::async_write(
                conn->mutable_socket(), *res,
                net::bind_executor(strand, [conn, res](beast::error_code write_ec, std::size_t) {
                  if (write_ec) spdlog::warn("HTTP write error: {}", write_ec.message());
                  // The request is answered, so the connection ends with it.
                  conn->Close();
                }));
          } catch (const std::exception& e) {
            spdlog::error("request failed: {}", e.what());
            conn->Close();
          }
        }));

    acceptor.async_accept(ioc, do_accept);
  };

  acceptor.async_accept(ioc, do_accept);
  std::thread ioc_thread([&] {
    // Every handler a socket runs is posted here, and a handler that throws would leave
    // through this thread's entry function, which is a terminate. A bad request would take
    // the server down with it; instead the loop is restarted, and a fault that keeps
    // recurring is logged rather than hidden.
    for (;;) {
      try {
        ioc.run();
        return;
      } catch (const std::exception& e) {
        spdlog::error("connection handler failed: {}", e.what());
      } catch (...) {
        spdlog::error("connection handler failed with a value that is not an exception");
      }
    }
  });

  while (!pu::platform::ShutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  {
    beast::error_code ec;
    acceptor.close(ec);
  }
  ioc.stop();
  if (ioc_thread.joinable()) ioc_thread.join();
  runner->Stop();
  config::UnregisterServeRecord(platform::CurrentProcessId());

  runtime.Shutdown();
  return 0;
}

}  // namespace pu::cli
