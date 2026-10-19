// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pu/core/base.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/serve/board.hpp"

namespace pu::cli::detail {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class SessionRunner;

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex, SessionRunner* runner,
                         serve::Board* board, http::request<http::string_body>&& req,
                         http::response<http::string_body>& res);

// The directory the running executable lives in, or empty when it cannot be determined. Kept
// beside the request dispatch so the web/ lookup above it and its tests agree on one answer.
std::filesystem::path ExecutableDir();

// Where the web assets live. Honours PU_WEB_DIR, then searches beside the executable, then the
// usual install locations, and finally falls back to ./web.
const std::string& GetWebDir();

class SessionRunner;

class WebSocketConnection : public std::enable_shared_from_this<WebSocketConnection> {
 public:
  WebSocketConnection(tcp::socket socket, net::strand<net::io_context::executor_type> strand,
                      Runtime& runtime, SessionRunner& runner);
  ~WebSocketConnection() = default;

  void Start(http::request<http::string_body> req);
  void Send(boost::json::value frame);

  // Close from any thread: the stream belongs to its strand, so the teardown is posted there.
  void Close();

  tcp::socket& mutable_socket() { return ws_.next_layer(); }

  // The tab's own name for itself, read from `?client=` on the upgrade. Empty when the client
  // did not name one, which makes every such connection a page of its own.
  const std::string& ClientId() const { return client_id_; }

  // Both flags are read from another connection's strand when a page takes the session over,
  // so they are atomic rather than plain.
  bool AttachedToPage() const { return attached_.load(std::memory_order_acquire); }
  void AttachToPage() { attached_.store(true, std::memory_order_release); }
  void DetachFromPage() { attached_.store(false, std::memory_order_release); }
  bool Closed() const { return closed_.load(std::memory_order_acquire); }

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
  std::string client_id_;
  bool writing_ = false;
  std::atomic<bool> closed_{false};
  std::atomic<bool> attached_{false};
  bool close_after_write_ = false;
};

class SessionRunner {
 public:
  SessionRunner(Runtime& runtime);
  ~SessionRunner();

  SessionRunner(const SessionRunner&) = delete;
  SessionRunner& operator=(const SessionRunner&) = delete;

  void Submit(std::string text, std::shared_ptr<WebSocketConnection> conn);
  // A turn that arrived from a peer has no page to answer: its reply goes back through the
  // send_to_peer tool, so the runner runs it without a connection. The turn id comes back so
  // the acceptance can name the turn the sender should watch.
  std::string SubmitFromPeer(std::string sender, std::string text, std::string job = {});
  void AttachBoard(serve::Board* board) { board_ = board; }
  void Stop();

  void CancelTurn();


  bool Attach(const std::shared_ptr<WebSocketConnection>& conn);
  void Detach(const std::shared_ptr<WebSocketConnection>& conn);

  // The frames of one turn, as they were delivered. A page that was not attached while the
  // turn ran reads them here instead of waiting for the next reload: this is what an
  // event-stream subscriber gets, and what a reconnecting page is handed in `Attach`.
  //
  // `frames` is the whole turn as it happened, and `finished` says the `done` frame is in it.
  // A subscriber records how many it has seen and is given the rest, which is what makes a
  // reconnect mid-turn cost nothing.
  struct TurnRecord {
    std::string id;
    bool finished = false;
    std::deque<std::shared_ptr<const std::string>> frames;
  };
  // The record for `turn_id`, or nullptr when it is no longer kept. The shared_ptr is the
  // record itself: a subscriber holds it and reads `frames` under `frame_mutex_`, which is
  // also the lock the worker appends under.
  std::shared_ptr<TurnRecord> RecordOf(const std::string& turn_id) const;
  std::mutex& FrameMutex() const { return frame_mutex_; }

 private:
  void WorkerMain();
  // `by_page` is whether the page asked for this, which decides if the turn still answers.
  void RequestCancel(bool by_page);
  // The frames a page attaching now should be handed: the turn in flight and the last
  // finished one, so a reply that arrived with nobody watching is drawn without a reload.
  std::deque<std::shared_ptr<const std::string>> FramesForNextPage() const;
  // Frames of the turn in flight and of the last few finished ones, newest last. Bounded so a
  // long-lived server does not keep every byte it ever sent; an older turn is still readable
  // from /api/history, which carries the result rather than the stream.
  //
  // `turns_mutex_` guards the list itself (which records exist). `frame_mutex_` guards the
  // frames inside one record, since a subscriber reads them while the worker appends.
  mutable std::mutex turns_mutex_;
  mutable std::mutex frame_mutex_;
  std::deque<std::shared_ptr<TurnRecord>> turns_;
  static constexpr std::size_t kTurnsKept = 8;
  void AppendFrame(boost::json::value frame);
  void FinishTurn();

  // The one token pair a turn runs under, swapped in when the job starts and out when it
  // ends. A socket that attaches mid-turn takes a copy and flips the same flags, so a page
  // that reconnects can still cancel or stop what its previous socket started.
  struct Tokens {
    CancelToken cancel;

  };
  Tokens CurrentTokens() const;

  Runtime& runtime_;
  serve::Board* board_ = nullptr;

  struct Job {
    std::string text;
    std::shared_ptr<WebSocketConnection> conn;
    std::string peer_origin;
    // The turn this job runs, so its record can be found while it is still being written.
    std::string turn_id;
    // The board job this turn answers, when a peer posted one. Closing it is this turn's job.
    std::string board_job;
  };
  void CloseBoardJob(const std::string& job_id, const ExecutionResult& result);
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  bool running_ = false;
  std::thread worker_;

  std::mutex attach_mutex_;
  std::weak_ptr<WebSocketConnection> attached_;

  mutable std::mutex turn_mutex_;
  CancelToken cancel_token_;

  // A `cancel` frame is the page asking for its turn to stop, which still ends with `done`.
  // A turn cancelled because its page went away reports nothing at all.
  std::atomic<bool> cancel_requested_{false};
};

}  // namespace pu::cli::detail
