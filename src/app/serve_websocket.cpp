// SPDX-License-Identifier: GPL-3.0-only

//
// One WebSocket connection: the upgrade, the frames it is handed and the frames it reads.
//
// The connection owns a socket and nothing else. It does not know what a turn is -- `run`,
// `cancel` and the rest are forwarded to the runner -- and it keeps no history: a frame it
// is asked to send is written or dropped. That is what lets this file be read as the
// transport it is, without the turn's states in between.
//

#include "serve_internal.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pu/core/json.hpp"

namespace pu::cli::detail {

namespace beast = boost::beast;
namespace websocket = beast::websocket;

namespace {

// The `client` query parameter names the tab that opened the socket. A reload and a
// reconnect keep the name; a second tab has one of its own. The value is only ever compared
// with another client's, so it is read as an opaque string.
std::string ClientIdFromTarget(std::string_view target) {
  const auto query = target.find('?');
  if (query == std::string_view::npos) return {};
  std::string_view rest = target.substr(query + 1);
  constexpr std::string_view kKey = "client=";
  while (!rest.empty()) {
    const auto end = rest.find('&');
    const std::string_view pair = rest.substr(0, end);
    if (pair.substr(0, kKey.size()) == kKey) return std::string(pair.substr(kKey.size()));
    if (end == std::string_view::npos) break;
    rest = rest.substr(end + 1);
  }
  return {};
}

}  // namespace

WebSocketConnection::WebSocketConnection(tcp::socket socket,
                                         net::strand<net::io_context::executor_type> strand,
                                         Runtime& runtime, SessionRunner& runner)
    : ws_(std::move(socket)), strand_(std::move(strand)), runtime_(runtime), runner_(runner) {}

void WebSocketConnection::Start(http::request<http::string_body> req) {
  auto self = shared_from_this();
  client_id_ = ClientIdFromTarget(req.target());
  // The suggested server timeouts ping after 300 idle seconds, which is longer than the
  // NAT and proxy idle windows a browser page routinely sits through between turns. The
  // page answers pings itself, so the interval can be short; a connection the page gave
  // up on without a close frame is read error and reaped either way.
  ws_.set_option(websocket::stream_base::timeout{
      .handshake_timeout = std::chrono::seconds(30),
      .idle_timeout = std::chrono::seconds(30),
      .keep_alive_pings = true,
  });
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
                     self->AttachToPage();
                     self->DoRead();
                   }));
}

void WebSocketConnection::Close() {
  auto self = shared_from_this();
  net::post(strand_, [self] { self->Finish(); });
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
  SendText(std::make_shared<const std::string>(boost::json::serialize(frame)));
}

void WebSocketConnection::SendText(std::shared_ptr<const std::string> text) {
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
  if (closed_.exchange(true, std::memory_order_acq_rel)) return;
  write_queue_.clear();
  DetachFromPage();
  runner_.Detach(shared_from_this());
  beast::error_code ignore;
  // Valid whether or not the upgrade happened: a connection closed before its handshake
  // still owns the socket this shuts down.
  ws_.next_layer().close(ignore);
}

}  // namespace pu::cli::detail
