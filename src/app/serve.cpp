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

using detail::DispatchHttpRequest;
using detail::SessionRunner;
using detail::WebSocketConnection;

}  // namespace

namespace detail {

WebSocketConnection::WebSocketConnection(tcp::socket socket,
                                         net::strand<net::io_context::executor_type> strand,
                                         Runtime& runtime, SessionRunner& runner)
    : ws_(std::move(socket)), strand_(std::move(strand)), runtime_(runtime), runner_(runner) {}

void WebSocketConnection::Start(http::request<http::string_body> req) {
  auto self = shared_from_this();
  ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  ws_.async_accept(req, net::bind_executor(strand_, [self](beast::error_code ec) {
                     if (ec) {
                       spdlog::warn("WebSocket accept error: {}", ec.message());
                       self->Finish();
                       return;
                     }
                     if (!self->runner_.Attach(self)) {
                       self->Send({{"type", "busy"},
                                   {"payload",
                                    {{"text", "This session is already open in another page."}}}});
                       self->SendPendingThenClose();
                       return;
                     }
                     self->SetAttached(true);
                     self->DoRead();
                   }));
}

void WebSocketConnection::SendPendingThenClose() {
  auto self = shared_from_this();
  net::post(strand_, [self] {
    if (!self->writing_ && !self->write_queue_.empty()) self->DoWrite();
    self->close_after_write_ = true;
    if (!self->writing_) self->Finish();
  });
}

void WebSocketConnection::Send(boost::json::value frame) {
  auto text = std::make_shared<const std::string>(boost::json::serialize(frame));
  auto self = shared_from_this();
  net::post(strand_, [self, text] {
    if (self->closed_) return;
    self->write_queue_.push_back(text);
    if (!self->writing_) self->DoWrite();
  });
}

void WebSocketConnection::DoRead() {
  read_buffer_.consume(read_buffer_.size());
  auto self = shared_from_this();
  ws_.async_read(read_buffer_, net::bind_executor(strand_, [self](beast::error_code ec,
                                                                  std::size_t bytes) {
                   self->OnRead(ec, bytes);
                 }));
}

void WebSocketConnection::OnRead(beast::error_code ec, std::size_t) {
  if (ec) {
    if (ec != websocket::error::closed && ec != net::error::operation_aborted) {
      spdlog::warn("WebSocket read error: {}", ec.message());
    }
    Finish();
    return;
  }

  const std::string text = beast::buffers_to_string(read_buffer_.data());
  boost::json::value jv;
  try {
    jv = boost::json::parse(text);
  } catch (const boost::system::system_error& e) {
    Send({{"type", "error"}, {"payload", {{"text", std::string("Invalid JSON: ") + e.what()}}}});
    DoRead();
    return;
  }

  if (jv.is_object()) {
    try {
      HandleMessage(jv);
    } catch (const std::exception& e) {
      spdlog::warn("WebSocket message error: {}", e.what());
    }
  }
  DoRead();
}

void WebSocketConnection::HandleMessage(const boost::json::value& jv) {
  const std::string type = json::ValueOrDefault<std::string>(jv, "type", "");
  const auto payload =
      json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{});

  if (type == "run") {
    const std::string text = json::ValueOrDefault<std::string>(payload, "text", "");
    if (!text.empty()) runner_.Submit(text, shared_from_this());
    return;
  }
  if (type == "cancel") {
    runner_.CancelTurn();
    return;
  }
  if (type == "note") {
    const std::string note = json::ValueOrDefault<std::string>(payload, "text", "");
    if (!note.empty()) runtime_.SetUserNote(note);
    return;
  }
  if (type == "stop_command") {
    const std::string note = json::ValueOrDefault<std::string>(payload, "text", "");
    runner_.StopCommand(note);
    return;
  }
  if (type == "resolve_approval") {
    const bool allow = json::ValueOrDefault<bool>(payload, "allow", false);
    const std::string note = json::ValueOrDefault<std::string>(payload, "text", "");
    runtime_.ResolveApproval(allow, note);
    return;
  }
}

void WebSocketConnection::DoWrite() {
  if (closed_ || write_queue_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  auto self = shared_from_this();
  ws_.text(true);
  ws_.async_write(net::buffer(*write_queue_.front()),
                  net::bind_executor(strand_, [self](beast::error_code ec, std::size_t bytes) {
                    self->OnWrite(ec, bytes);
                  }));
}

void WebSocketConnection::OnWrite(beast::error_code ec, std::size_t) {
  writing_ = false;
  if (ec) {
    Finish();
    return;
  }
  write_queue_.pop_front();
  if (write_queue_.empty() && close_after_write_) {
    Finish();
    return;
  }
  DoWrite();
}

void WebSocketConnection::Finish() {
  if (closed_) return;
  closed_ = true;
  write_queue_.clear();
  runner_.Detach(shared_from_this());
  beast::error_code ignore;
  ws_.next_layer().close(ignore);
}

SessionRunner::SessionRunner(Runtime& runtime) : runtime_(runtime) {
  worker_ = std::thread([this] { WorkerMain(); });
}

SessionRunner::~SessionRunner() { Stop(); }

bool SessionRunner::Attach(const std::shared_ptr<WebSocketConnection>& conn) {
  std::lock_guard<std::mutex> lock(attach_mutex_);
  auto current = attached_.lock();
  if (current && current != conn) return false;
  attached_ = conn;
  return true;
}

void SessionRunner::Detach(const std::shared_ptr<WebSocketConnection>& conn) {
  std::lock_guard<std::mutex> lock(attach_mutex_);
  if (auto current = attached_.lock(); current == conn) {
    CancelTurn();
    attached_.reset();
  }
}

void SessionRunner::Submit(std::string text, std::shared_ptr<WebSocketConnection> conn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(Job{std::move(text), std::move(conn)});
  }
  cv_.notify_one();
}

void SessionRunner::Stop() {
  CancelToken cancel;
  CancelToken command_stop;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    jobs_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    cancel = cancel_token_;
    command_stop = command_stop_;
  }
  if (cancel) cancel->store(true);
  if (command_stop) command_stop->store(true);
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void SessionRunner::CancelTurn() {
  std::lock_guard<std::mutex> lock(turn_mutex_);
  if (cancel_token_) cancel_token_->store(true);
}

void SessionRunner::StopCommand(const std::string& note) {
  if (!note.empty()) runtime_.SetUserNote(note);
  std::lock_guard<std::mutex> lock(turn_mutex_);
  if (command_stop_) command_stop_->store(true);
}

void SessionRunner::Deliver(const std::shared_ptr<WebSocketConnection>& conn,
                            boost::json::value frame) {
  if (conn) conn->Send(std::move(frame));
}

void SessionRunner::WorkerMain() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_ && jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    auto conn = job.conn;
    if (!conn) continue;

    const auto say = [&](boost::json::value frame) { Deliver(conn, std::move(frame)); };

    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      cancel_token_ = std::make_shared<std::atomic<bool>>(false);
      command_stop_ = std::make_shared<std::atomic<bool>>(false);
    }

    bool is_command = false;
    ToolCallbacks tool_cb;
    tool_cb.on_start = [&](const std::string& id, const std::string& name,
                           const boost::json::value& args) {
      say({{"type", "tool_start"}, {"payload", {{"id", id}, {"name", name}, {"args", args}}}});
    };
    tool_cb.on_end = [&](const std::string& id, const std::string& output,
                         const std::string& error) {
      say({{"type", "tool_end"}, {"payload", {{"id", id}, {"output", output}, {"error", error}}}});
    };

    runtime_.EnableApprovalGate();
    runtime_.SetOnApprovalAsk([&](const std::string& command) {
      say({{"type", "approval_ask"}, {"payload", {{"command", command}}}});
    });

    CancelToken cancel;
    CancelToken command_stop;
    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      cancel = cancel_token_;
      command_stop = command_stop_;
    }

    ExecutionResult result = runtime_.ProcessInput(
        job.text, is_command, cancel,
        [&](std::string_view chunk) {
          if (chunk.empty()) return;
          say({{"type", "chunk"}, {"payload", {{"text", std::string(chunk)}}}});
        },
        tool_cb,
        [&](std::string_view thought) {
          if (thought.empty()) return;
          say({{"type", "thinking"}, {"payload", {{"text", std::string(thought)}}}});
        },
        command_stop);

    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      cancel_token_.reset();
      command_stop_.reset();
    }

    if (!result.notice.empty()) {
      say({{"type", "notice"}, {"payload", {{"text", result.notice}}}});
    }
    if (result.usage) {
      say({{"type", "usage"},
           {"payload",
            {{"prompt_tokens", result.usage->prompt_tokens},
             {"completion_tokens", result.usage->completion_tokens}}}});
    }
    if (result.has_error) {
      say({{"type", "error"}, {"payload", {{"text", result.error_message}}}});
    } else if (result.model.empty()) {
      say({{"type", "done"}});
    } else {
      say({{"type", "done"}, {"payload", {{"model", result.model}}}});
    }
  }
}

}  // namespace detail

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
  const tcp::endpoint listen_endpoint(net::ip::make_address(host), port);
  tcp::acceptor acceptor(ioc);
  {
    beast::error_code open_ec;
    acceptor.open(listen_endpoint.protocol(), open_ec);
    if (open_ec) {
      spdlog::error("Cannot open {}:{}: {}", host, port, open_ec.message());
      return 1;
    }
    acceptor.set_option(net::socket_base::reuse_address(true), open_ec);
    if (open_ec) spdlog::warn("Cannot set reuse_address: {}", open_ec.message());
    acceptor.bind(listen_endpoint, open_ec);
    if (open_ec) {
      spdlog::error("Cannot bind {}:{}: {}", host, port, open_ec.message());
      return 1;
    }
    acceptor.listen(net::socket_base::max_listen_connections, open_ec);
    if (open_ec) {
      spdlog::error("Cannot listen on {}:{}: {}", host, port, open_ec.message());
      return 1;
    }
  }

  std::mutex io_mutex;
  auto runner = std::make_shared<SessionRunner>(runtime);

  std::function<void(beast::error_code, tcp::socket)> do_accept = [&](beast::error_code ec,
                                                                      tcp::socket socket) {
    if (ec) {
      if (ec != net::error::operation_aborted) spdlog::warn("Accept error: {}", ec.message());
      return;
    }

    auto strand = net::make_strand(ioc);
    auto conn = std::make_shared<WebSocketConnection>(std::move(socket), strand, runtime, *runner);
    net::post(strand, [&, conn] {
      auto buffer = std::make_shared<beast::flat_buffer>();
      auto req = std::make_shared<http::request<http::string_body>>();
      tcp::socket& sock = conn->mutable_socket();
      beast::error_code read_ec;
      http::read(sock, *buffer, *req, read_ec);
      if (read_ec) {
        spdlog::warn("HTTP read error: {}", read_ec.message());
        return;
      }
      if (websocket::is_upgrade(*req)) {
        conn->Start(std::move(*req));
      } else {
        auto res = std::make_shared<http::response<http::string_body>>();
        DispatchHttpRequest(runtime, io_mutex, std::move(*req), *res);
        beast::error_code write_ec;
        http::write(sock, *res, write_ec);
        if (write_ec) spdlog::warn("HTTP write error: {}", write_ec.message());
      }
    });

    acceptor.async_accept(ioc, do_accept);
  };

  acceptor.async_accept(ioc, do_accept);
  std::thread ioc_thread([&] { ioc.run(); });

  while (!pu::platform::IsInterrupted()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  {
    beast::error_code ec;
    acceptor.close(ec);
  }
  ioc.stop();
  if (ioc_thread.joinable()) ioc_thread.join();
  runner->Stop();

  runtime.Shutdown();
  return 0;
}

}  // namespace pu::cli
