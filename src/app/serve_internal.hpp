// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
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

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res);

class SessionRunner;

class WebSocketConnection : public std::enable_shared_from_this<WebSocketConnection> {
 public:
  WebSocketConnection(tcp::socket socket, net::strand<net::io_context::executor_type> strand,
                      Runtime& runtime, SessionRunner& runner);
  ~WebSocketConnection() = default;

  void Start(http::request<http::string_body> req);
  void Send(boost::json::value frame);

  tcp::socket& mutable_socket() { return ws_.next_layer(); }

  bool IsAttached() const { return attached_; }
  void SetAttached(bool attached) { attached_ = attached; }

 private:
  void DoRead();
  void OnRead(beast::error_code ec, std::size_t bytes);
  void HandleMessage(const boost::json::value& jv);
  void DoWrite();
  void OnWrite(beast::error_code ec, std::size_t bytes);
  void SendPendingThenClose();
  void Finish();

  websocket::stream<tcp::socket> ws_;
  net::strand<net::io_context::executor_type> strand_;
  Runtime& runtime_;
  SessionRunner& runner_;
  beast::flat_buffer read_buffer_;
  std::deque<std::shared_ptr<const std::string>> write_queue_;
  bool writing_ = false;
  bool closed_ = false;
  bool attached_ = false;
  bool close_after_write_ = false;
};

class SessionRunner {
 public:
  SessionRunner(Runtime& runtime);
  ~SessionRunner();

  SessionRunner(const SessionRunner&) = delete;
  SessionRunner& operator=(const SessionRunner&) = delete;

  void Submit(std::string text, std::shared_ptr<WebSocketConnection> conn);
  void Stop();

  void CancelTurn();
  void StopCommand(const std::string& note);

  bool Attach(const std::shared_ptr<WebSocketConnection>& conn);
  void Detach(const std::shared_ptr<WebSocketConnection>& conn);

 private:
  void WorkerMain();
  void Deliver(const std::shared_ptr<WebSocketConnection>& conn, boost::json::value frame);

  Runtime& runtime_;

  struct Job {
    std::string text;
    std::shared_ptr<WebSocketConnection> conn;
  };
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  std::thread worker_;

  std::mutex attach_mutex_;
  std::weak_ptr<WebSocketConnection> attached_;

  std::mutex turn_mutex_;
  CancelToken cancel_token_;
  CancelToken command_stop_;
};

}  // namespace pu::cli::detail
