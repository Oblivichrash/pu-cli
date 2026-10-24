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
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "pu/core/json.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/config/serve_registry.hpp"
#include "pu/runtime/executor.hpp"
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
using detail::IsLoopback;
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

namespace detail {

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

SessionRunner::SessionRunner(Runtime& runtime) : runtime_(runtime) {
  worker_ = std::thread([this] { WorkerMain(); });
}

SessionRunner::~SessionRunner() { Stop(); }

bool SessionRunner::Attach(const std::shared_ptr<WebSocketConnection>& conn) {
  std::shared_ptr<WebSocketConnection> replaced;
  std::deque<std::shared_ptr<const std::string>> pending;
  {
    std::lock_guard<std::mutex> lock(attach_mutex_);
    auto current = attached_.lock();
    if (current && current != conn && !current->Closed() && current->AttachedToPage()) {
      // A live page keeps the session, and a second tab is refused rather than taking it
      // over. The same tab is not a second page: a reload, or the page reopening a socket
      // its watchdog gave up on, carries the name it already had, so it takes the session
      // back instead of being told another page holds it.
      if (conn->ClientId().empty() || conn->ClientId() != current->ClientId()) return false;
      spdlog::info("Replacing this page's earlier socket");
      replaced = current;
    }
    if (current && current != conn) current->DetachFromPage();
    attached_ = conn;
    // The turn may still be running for the socket that went away, or may have run while no
    // page was open at all (a turn a peer asked). The page is handed the frames of the turn in
    // flight so it joins in, and of the last finished one so a reply that arrived while nobody
    // was watching is drawn without a reload.
    pending = FramesForNextPage();
  }
  // Outside the lock: the old socket's teardown posts to its own strand and takes this mutex
  // again on the way out.
  if (replaced) replaced->Close();
  for (const auto& frame : pending) conn->Send(boost::json::parse(*frame));
  return true;
}

void SessionRunner::Detach(const std::shared_ptr<WebSocketConnection>& conn) {
  bool left_alone = false;
  {
    std::lock_guard<std::mutex> lock(attach_mutex_);
    if (auto current = attached_.lock(); current == conn) {
      attached_.reset();
      left_alone = true;
    }
  }
  // The page went away with nobody taking its place, so the turn it started has no reader:
  // it is cancelled and keeps nothing. A socket that a reconnect replaced is not that case,
  // which is why the check is for the connection still being the attached one.
  if (left_alone) RequestCancel(false);
}

std::shared_ptr<SessionRunner::TurnRecord> SessionRunner::RecordOf(
    const std::string& turn_id) const {
  std::lock_guard<std::mutex> lock(turns_mutex_);
  for (auto it = turns_.rbegin(); it != turns_.rend(); ++it) {
    if ((*it)->id == turn_id) return *it;
  }
  return nullptr;
}

void SessionRunner::AppendFrame(boost::json::value frame) {
  std::string wire = boost::json::serialize(frame);
  // A WebSocket text frame must be valid UTF-8, and a failure the transport reported carries
  // the platform's own message, which on Windows is in the console codepage rather than UTF-8.
  // Sending it raw used to make the frame invalid and the client drop the socket, so a turn
  // that failed looked like a page that had gone away.
  if (!text::IsValidUtf8(wire)) wire = text::SanitizeUtf8(wire);
  auto text = std::make_shared<const std::string>(std::move(wire));
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!turns_.empty()) turns_.back()->frames.push_back(text);
  }
  // A page that is here sees the turn as it happens. One that attaches later is handed the
  // record instead, which is also what a subscriber reads: a turn that arrived from a peer has
  // no page of its own, and waiting for a reload was the whole reason this record exists.
  std::shared_ptr<WebSocketConnection> current;
  {
    std::lock_guard<std::mutex> lock(attach_mutex_);
    current = attached_.lock();
  }
  // The frame is already its wire form. Parsing it back only to have the connection serialise
  // it again would put a throwing call on the worker thread for no gain.
  if (current && !current->Closed()) current->SendText(text);
}

void SessionRunner::FinishTurn() {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (!turns_.empty()) turns_.back()->finished = true;
}

void SessionRunner::Submit(std::string text, std::shared_ptr<WebSocketConnection> conn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Built field by field rather than with a brace-init list: the list has to name every
    // member, and the one that is forgotten is a warning on every build rather than a change
    // a reader sees.
    Job job;
    job.text = std::move(text);
    job.conn = std::move(conn);
    job.turn_id = session::NewMessageId();
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

std::string SessionRunner::SubmitFromPeer(std::string sender, std::string text,
                                          std::string job) {
  std::string turn_id = session::NewMessageId();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(Job{std::move(text), nullptr, std::move(sender), turn_id, std::move(job)});
  }
  cv_.notify_one();
  return turn_id;
}

void SessionRunner::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    jobs_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    if (cancel_token_) cancel_token_->store(true);
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

SessionRunner::Tokens SessionRunner::CurrentTokens() const {
  std::lock_guard<std::mutex> lock(turn_mutex_);
  return Tokens{cancel_token_};
}

void SessionRunner::CancelTurn() { RequestCancel(true); }

void SessionRunner::RequestCancel(bool by_page) {
  std::lock_guard<std::mutex> lock(turn_mutex_);
  // Cancelling with no turn in flight asks for nothing: the flag would otherwise be read by
  // the next turn, which nobody asked to stop.
  if (!cancel_token_) return;
  // Only a cancel the page asked for is answered with `done`; one that follows a page
  // going away is silent, since there is nobody left to tell.
  if (by_page) cancel_requested_.store(true, std::memory_order_release);
  cancel_token_->store(true);
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
    // A turn is the boundary an error stops at. Anything it throws is the turn's own failure,
    // and a worker that let it out would take the whole server with it -- including every
    // page and every peer turn that had nothing to do with it.
    try {
      RunJob(job);
    } catch (const std::exception& e) {
      spdlog::error("turn failed: {}", e.what());
      ReportFailedTurn(job, e.what());
    }
  }
}

void SessionRunner::ReportFailedTurn(const Job& job, const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  try {
    AppendFrame({{"type", "error"}, {"payload", {{"text", reason}}}});
    AppendFrame({{"type", "done"}, {"payload", boost::json::object{}}});
    FinishTurn();
  } catch (const std::exception& e) {
    spdlog::error("could not report a failed turn: {}", e.what());
  }
  if (!job.board_job.empty()) {
    try {
      CloseBoardJob(job.board_job, ExecutionResult{});
    } catch (const std::exception& e) {
      spdlog::error("could not close the board job of a failed turn: {}", e.what());
    }
  }
}

void SessionRunner::RunJob(Job& job) {
  {
    // A new record per turn: the frames of the one that ran before it are kept beside it, so
    // a subscriber can still read a turn that ended while nobody was watching.
    std::lock_guard<std::mutex> lock(turns_mutex_);
    auto record = std::make_shared<TurnRecord>();
    record->id = job.turn_id;
    turns_.push_back(std::move(record));
    while (turns_.size() > kTurnsKept) turns_.pop_front();
  }

  if (!job.conn && job.peer_origin.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }

  const auto say = [&](boost::json::value frame) { AppendFrame(std::move(frame)); };

  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    cancel_token_ = std::make_shared<std::atomic<bool>>(false);
    cancel_requested_.store(false, std::memory_order_release);
  }

  bool is_command = false;
  ToolCallbacks tool_cb;
  tool_cb.on_start = [&](const std::string& id, const std::string& name,
                         const boost::json::value& args) {
    say({{"type", "tool_start"}, {"payload", {{"id", id}, {"name", name}, {"args", args}}}});
  };
  tool_cb.on_end = [&](const std::string& id, const std::string& name,
                       const boost::json::value& args, const std::string& output,
                       const std::string& error) {
    say({{"type", "tool_end"},
         {"payload",
          {{"id", id}, {"name", name}, {"args", args}, {"output", output}, {"error", error}}}});
  };

  runtime_.EnableApprovalGate();
  runtime_.SetOnApprovalAsk([&](const std::string& command) {
    say({{"type", "approval_ask"}, {"payload", {{"command", command}}}});
  });

  CancelToken cancel;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    cancel = cancel_token_;
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
      cancel, job.peer_origin);

  const bool page_asked = cancel_requested_.exchange(false, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    cancel_token_.reset();
  }

  // A turn the page cancelled still answers with `done`, so the page can stop waiting. A
  // turn whose page went away keeps nothing: the store holds only the question, so a
  // replay of chunks and a done would draw a reply the next load then takes away.
  if (cancel && cancel->load(std::memory_order_acquire)) {
    const bool page_is_here = job.conn && job.conn->AttachedToPage();
    if (!page_asked || !page_is_here) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
      }
      FinishTurn();
      return;
    }
  }

  if (!result.notice.empty()) {
    say({{"type", "notice"}, {"payload", {{"text", result.notice}}}});
  }
  if (result.usage) {
    // The turn is already filed against the reply it produced, which is the entry
    // `/api/history` will replay. Reading it here is what makes a live turn and a reloaded
    // one the same object: one projection, written by the ledger, and no second assembly
    // of the same six fields to drift out of step with it.
    boost::json::object usage;
    session::SessionUsageTotal total;
    if (const auto session = runtime_.DefaultSession()) {
      // `Chain()` hands back the vector by value. Taken twice, `rbegin()` and `rend()` would
      // bound two different sequences -- the first already gone when the second is asked for.
      const std::vector<const session::MessageNode*> chain = session->Messages().Chain();
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!std::holds_alternative<session::AssistantPayload>((*it)->payload)) continue;
        if (const auto recorded = session->Usage().For((*it)->id)) {
          usage = session::UsageLedger::ToJson(*recorded).as_object();
        }
        break;
      }
      // The total `/api/session` would answer with is already known. Sending it means a page
      // that has just watched a turn cannot draw a figure that stops short of it, and does
      // not have to ask a second time to find out.
      total = session->Usage().Total();
    }
    usage["total"] = session::UsageLedger::ToJson(total);
    say({{"type", "usage"}, {"payload", std::move(usage)}});
  }
  if (result.has_error) {
    // The message a turn carries sits on `content` when the turn answered with its
    // own refusal (the wrap-up that never answered), and on `error_message` when the
    // failure came from below. Send whichever holds text, so the page never shows
    // "Unknown error" over an error the server did describe.
    const std::string& text = !result.error_message.empty() ? result.error_message : result.content;
    say({{"type", "error"}, {"payload", {{"text", text}}}});
  } else if (result.model.empty()) {
    say({{"type", "done"}});
  } else {
    say({{"type", "done"}, {"payload", {{"model", result.model}}}});
  }

  CloseBoardJob(job.board_job, result);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  // The record stays, finished: a page that attaches next is handed it rather than waiting
  // for a reload, and a subscriber that was mid-stream reads the end of it.
  FinishTurn();
}

void SessionRunner::CloseBoardJob(const std::string& job_id, const ExecutionResult& result) {
  if (job_id.empty() || board_ == nullptr) return;
  const auto& peers = runtime_.Peers();
  const std::string self = peers.has_value() ? peers->self_name : std::string();
  if (self.empty()) return;

  serve::JobRequest close;
  close.job = job_id;
  close.from = self;
  close.state = std::string{serve::JobStateName(result.has_error ? serve::JobState::kFailed
                                                                : serve::JobState::kDone)};
  close.result = result.has_error
                     ? (!result.error_message.empty() ? result.error_message : result.content)
                     : result.content;
  if (!board_->Update(close).outcome.has_value()) {
    spdlog::warn("Peer job '{}' could not be closed: it is no longer this server's", job_id);
  }
}

std::deque<std::shared_ptr<const std::string>> SessionRunner::FramesForNextPage() const {
  std::deque<std::shared_ptr<const std::string>> frames;
  std::lock_guard<std::mutex> list_lock(turns_mutex_);
  std::lock_guard<std::mutex> frame_lock(frame_mutex_);
  if (turns_.empty()) return frames;
  // The turn in flight (or the last one that ran) plus the one before it when it has finished:
  // a single finished turn is the reply the page missed, and two gives an exchange a reader
  // can follow.
  const std::size_t from = turns_.size() >= 2 ? turns_.size() - 2 : 0;
  for (std::size_t i = from; i < turns_.size(); ++i) {
    for (const auto& frame : turns_[i]->frames) frames.push_back(frame);
  }
  return frames;
}

}  // namespace detail

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
