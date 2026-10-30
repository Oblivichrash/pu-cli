// SPDX-License-Identifier: GPL-3.0-only

//
// The turn runner: which page holds the session, which turn is running, and what it has said.
//
// A connection forwards `run` here and forgets it. What happens next is a job on a worker
// thread, a record of the frames it produced, and the rule that at most one turn runs at a
// time. The record is why a page that reconnects mid-turn sees the reply instead of waiting
// for a reload, and why a peer's turn -- which has no page at all -- can still be read.
//

#include "serve_internal.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "pu/core/text.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/serve/board.hpp"

namespace pu::cli::detail {

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

}  // namespace pu::cli::detail
