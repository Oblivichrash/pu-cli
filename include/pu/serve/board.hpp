// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/json.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace pu::serve {

// Where a job stands. `open` is a job nobody has taken; `claimed` is a peer saying it has the
// job in hand; `blocked` is a peer saying it cannot finish without an answer; `done` and
// `failed` are the two that close a job. A declined job is `open` again, carrying who declined
// it, so a decline costs no state of its own. `cancelled` is a job the person took back before
// anyone finished it, which closes it like `done` and `failed` do.
enum class JobState { kOpen, kClaimed, kWorking, kBlocked, kDone, kFailed, kCancelled };

const char* JobStateName(JobState state);
std::optional<JobState> ParseJobState(const std::string& name);

// The token `due` accepts. A duration is what a model can state honestly — "30m" is a
// commitment, an absolute timestamp is a guess at what time it is — so the board resolves it.
std::optional<std::chrono::seconds> ParseDuration(const std::string& text);

// One thing someone owes someone. A job exists from the moment it is posted and stays on the
// board until it closes, so a job that stops moving is visible rather than merely absent.
struct Job {
  std::string id;
  std::string from;
  std::string to;
  std::string intent;
  JobState state = JobState::kOpen;
  // Empty when the job carries no deadline, which is what an exploratory job wants.
  std::optional<std::string> due_at;
  std::string created_at;
  std::string updated_at;
  std::optional<std::string> result;
  std::optional<std::string> question;
  std::string note;
  // Who declined the job most recently, and how many times it has been declined.
  std::optional<std::string> declined_by;
  std::uint32_t declines = 0;

  // How many times a person asked the holder what is happening, which leaves the job where it
  // is and only says so.
  std::uint32_t chases = 0;

  bool Closed() const {
    return state == JobState::kDone || state == JobState::kFailed || state == JobState::kCancelled;
  }
};

// What a peer says about a job it holds, or what a person says to post one.
struct JobUpdate {
  std::string state;
  std::string result;
  std::string question;
  std::string note;
};

struct JobRequest {
  std::optional<std::string> job;
  std::optional<std::string> from;
  std::optional<std::string> to;
  std::optional<std::string> intent;
  std::optional<std::string> due;
  std::optional<std::string> state;
  std::optional<std::string> result;
  std::optional<std::string> question;
  std::optional<std::string> note;
};

// What a closed job ended as, which is the whole of what a caller waiting on it needs.
struct JobOutcome {
  std::string id;
  std::string state;
  std::optional<std::string> result;
};

// Every job, who is working on one, and how long ago each side last spoke. It is the only place
// agents and people share: an agent posts and updates, a person reads all of it and decides.
//
// Nothing here sends anything. A job past its deadline, or a peer that stopped reporting, is
// only a different answer from `Snapshot`; who reacts to that is a person, and a peer that was
// told would be interrupted for a fact it cannot act on.
class Board {
 public:
  struct ActionResult {
    Job job;
    // Attached when the update closed the job, so the caller does not re-read the board to
    // find out what happened.
    std::optional<JobOutcome> outcome;
  };

  static constexpr std::size_t kMaxOpen = 32;
  static constexpr std::size_t kMaxAudit = 64;
  static constexpr std::size_t kMaxClosedKept = 128;

  // A peer saying it is alive or doing something. `state` may be empty, which is a heartbeat.
  void TouchPeer(const std::string& peer, const std::string& state = {});
  // A job posted by a peer or by a person. `from` empty means the person.
  ActionResult Post(const JobRequest& request);
  // A peer moving a job it holds: claimed from open, working, blocked, done or failed.
  ActionResult Update(const JobRequest& request);
  // A person answering what a blocked job is waiting for. The person is the scheduler, not a
  // peer, so this is not `Update` with a borrowed name: it is the one verb no peer has.
  ActionResult Answer(const std::string& id, const std::string& answer);
  // A person asking a job that has not closed what is happening. The job stays where it is, so
  // this is not a state change: it is a note a peer reads in its digest, and a count on the job.
  ActionResult Chase(const std::string& id, const std::string& note);
  // A person taking a job back before it finishes. It closes as `cancelled` so the holder learns
  // from the board rather than from a message that may arrive after it has already finished.
  ActionResult Drop(const std::string& id, const std::string& note);

  std::optional<Job> Find(const std::string& id);

  // The board as JSON: every job, and every peer with its liveness and what it holds.
  json::value Snapshot(std::chrono::seconds offline_after) const;

  // What the board owes `peer`: the jobs it holds, what it is blocked on, and the jobs it
  // handed out that have since closed. Empty when there is nothing to say, so a session with
  // no peers in play reads no differently from one without a board.
  std::string Digest(const std::string& peer) const;

  // A job as the board serves it, which is also what a caller hands back to a model.
  static json::value Render(const Job& job);

 private:
  void Record(const std::string& kind, const std::string& peer, const std::string& job,
              const std::string& text);
  static void Move(Job& job, const JobUpdate& update, const std::string& now);
  static json::value Render(const Job& job, const std::string& now);
  static std::string NowStamp();
  static std::int64_t AgeSeconds(const std::string& stamp, const std::string& now);

  mutable std::mutex mutex_;
  // Newest last, so a snapshot reads in the order things happened.
  std::deque<Job> jobs_;
  struct PeerPresence {
    std::string name;
    std::string last_seen;
    std::string state;
    std::string job;
  };
  std::deque<PeerPresence> peers_;
  struct AuditEntry {
    std::string at;
    std::string kind;
    std::string peer;
    std::string job;
    std::string text;
  };
  std::deque<AuditEntry> audit_;
};

}  // namespace pu::serve
