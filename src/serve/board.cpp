// SPDX-License-Identifier: GPL-3.0-only
#include "pu/serve/board.hpp"

#include "pu/core/base.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <ctime>

namespace pu::serve {
namespace {

constexpr const char* kOpen = "open";
constexpr const char* kClaimed = "claimed";
constexpr const char* kWorking = "working";
constexpr const char* kBlocked = "blocked";
constexpr const char* kDone = "done";
constexpr const char* kFailed = "failed";

std::time_t Timegm(std::tm* utc) {
#if defined(_WIN32)
  return _mkgmtime(utc);
#else
  return timegm(utc);
#endif
}

std::string Stamp(std::chrono::system_clock::time_point at) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif

  std::array<char, 32> buffer{};
  const std::size_t written =
      std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buffer.data(), written);
}

std::optional<std::chrono::system_clock::time_point> ParseStamp(const std::string& text) {
  std::tm utc{};
  const int fields = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &utc.tm_year,
                                 &utc.tm_mon, &utc.tm_mday, &utc.tm_hour, &utc.tm_min,
                                 &utc.tm_sec);
  if (fields != 6) {
    return std::nullopt;
  }
  utc.tm_year -= 1900;
  utc.tm_mon -= 1;
  return std::chrono::system_clock::from_time_t(Timegm(&utc));
}

}  // namespace

const char* JobStateName(JobState state) {
  switch (state) {
    case JobState::kOpen:
      return kOpen;
    case JobState::kClaimed:
      return kClaimed;
    case JobState::kWorking:
      return kWorking;
    case JobState::kBlocked:
      return kBlocked;
    case JobState::kDone:
      return kDone;
    case JobState::kFailed:
      return kFailed;
  }
  return kOpen;
}

std::optional<JobState> ParseJobState(const std::string& name) {
  if (name == kOpen) return JobState::kOpen;
  if (name == kClaimed) return JobState::kClaimed;
  if (name == kWorking) return JobState::kWorking;
  if (name == kBlocked) return JobState::kBlocked;
  if (name == kDone) return JobState::kDone;
  if (name == kFailed) return JobState::kFailed;
  return std::nullopt;
}

std::optional<std::chrono::seconds> ParseDuration(const std::string& text) {
  if (text.empty()) return std::nullopt;

  std::size_t split = 0;
  while (split < text.size() && text[split] >= '0' && text[split] <= '9') ++split;
  if (split == 0) return std::nullopt;

  std::int64_t count = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + split, count);
  if (parsed.ec != std::errc{} || count <= 0) return std::nullopt;

  const std::string unit = text.substr(split);
  std::int64_t scale = 0;
  if (unit == "s") {
    scale = 1;
  } else if (unit == "m") {
    scale = 60;
  } else if (unit == "h") {
    scale = 3600;
  } else if (unit == "d") {
    scale = 86400;
  } else {
    return std::nullopt;
  }
  return std::chrono::seconds(count * scale);
}

std::string Board::NowStamp() { return Stamp(std::chrono::system_clock::now()); }

std::int64_t Board::AgeSeconds(const std::string& stamp, const std::string& now) {
  const auto then = ParseStamp(stamp);
  const auto current = ParseStamp(now);
  if (!then || !current) return 0;
  return std::chrono::duration_cast<std::chrono::seconds>(*current - *then).count();
}

json::value Board::Render(const Job& job, const std::string& now) {
  json::object out;
  out["id"] = job.id;
  out["from"] = job.from;
  out["to"] = job.to;
  out["intent"] = job.intent;
  out["state"] = JobStateName(job.state);
  out["created_at"] = job.created_at;
  out["updated_at"] = job.updated_at;
  out["idle_seconds"] = AgeSeconds(job.updated_at, now);
  if (job.due_at) {
    out["due_at"] = *job.due_at;
    out["overdue"] = !job.Closed() && AgeSeconds(*job.due_at, now) > 0;
  } else {
    out["overdue"] = false;
  }
  if (job.result) out["result"] = *job.result;
  if (job.question) out["question"] = *job.question;
  if (!job.note.empty()) out["note"] = job.note;
  if (job.declined_by) out["declined_by"] = *job.declined_by;
  if (job.declines > 0) out["declines"] = job.declines;
  return out;
}

json::value Board::Render(const Job& job) { return Render(job, NowStamp()); }

void Board::Record(const std::string& kind, const std::string& peer, const std::string& job,
                   const std::string& text) {
  audit_.push_back({NowStamp(), kind, peer, job, text});
  while (audit_.size() > kMaxAudit) audit_.pop_front();
}

void Board::TouchPeer(const std::string& peer, const std::string& state) {
  if (peer.empty()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string now = NowStamp();
  auto it = std::find_if(peers_.begin(), peers_.end(),
                         [&](const PeerPresence& p) { return p.name == peer; });
  if (it == peers_.end()) {
    peers_.push_back({peer, now, state, {}});
    return;
  }
  it->last_seen = now;
  if (!state.empty()) it->state = state;
}

Board::ActionResult Board::Post(const JobRequest& request) {
  const std::string to = request.to.value_or("");
  const std::string intent = request.intent.value_or("");
  if (to.empty()) throw RequestRefused("a job needs the peer it is for, as `to`");
  if (intent.empty()) throw RequestRefused("a job needs an `intent`");

  std::lock_guard<std::mutex> lock(mutex_);
  const auto open = std::count_if(jobs_.begin(), jobs_.end(),
                                  [](const Job& job) { return !job.Closed(); });
  if (static_cast<std::size_t>(open) >= kMaxOpen) {
    throw RequestRefused("the board already holds " + std::to_string(kMaxOpen) +
                         " unfinished jobs, so close one before posting another");
  }

  const std::string now = NowStamp();
  Job job;
  job.id = uuid::Generate();
  job.from = request.from.value_or("");
  job.to = to;
  job.intent = intent;
  job.state = JobState::kOpen;
  job.created_at = now;
  job.updated_at = now;
  if (request.due) {
    const auto span = ParseDuration(*request.due);
    if (!span) {
      throw RequestRefused("`due` is a duration such as \"30m\", \"2h\" or \"1d\", not " +
                           *request.due);
    }
    job.due_at = Stamp(std::chrono::system_clock::now() + *span);
  }

  jobs_.push_back(job);
  if (jobs_.size() > kMaxOpen + kMaxClosedKept) jobs_.pop_front();
  Record("post", job.from, job.id, job.intent);

  return {job, std::nullopt};
}

void Board::Move(Job& job, const JobUpdate& update, const std::string& now) {
  const auto next = ParseJobState(update.state);
  if (!next) {
    throw RequestRefused("`state` is open, claimed, working, blocked, done or failed, not " +
                         update.state);
  }

  switch (*next) {
    case JobState::kOpen:
      job.state = JobState::kOpen;
      job.due_at = std::nullopt;
      job.question = std::nullopt;
      job.result = std::nullopt;
      break;
    case JobState::kClaimed:
    case JobState::kWorking:
      job.state = *next;
      job.question = std::nullopt;
      job.declined_by = std::nullopt;
      break;
    case JobState::kBlocked:
      if (update.question.empty()) {
        throw RequestRefused("`blocked` needs a `question`, which is what the holder is asking");
      }
      job.state = JobState::kBlocked;
      job.question = update.question;
      break;
    case JobState::kDone:
      job.state = JobState::kDone;
      if (!update.result.empty()) job.result = update.result;
      break;
    case JobState::kFailed:
      job.state = JobState::kFailed;
      job.result = update.result.empty() ? std::string("failed") : update.result;
      break;
  }

  job.updated_at = now;
}

Board::ActionResult Board::Update(const JobRequest& request) {
  const std::string id = request.job.value_or("");
  const std::string peer = request.from.value_or("");
  if (id.empty()) throw RequestRefused("a job update needs the `job` it is about");
  if (peer.empty()) throw RequestRefused("a job update needs the `from` that holds it");

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
  if (it == jobs_.end()) throw NotFound("no job with id " + id);
  if (it->to != peer) {
    throw RequestRefused("job " + id + " is " + it->to + "'s, so " + peer + " cannot update it");
  }
  if (it->Closed()) {
    throw RequestRefused("job " + id + " already ended as " + JobStateName(it->state));
  }

  JobUpdate update{request.state.value_or(""), request.result.value_or(""),
                   request.question.value_or(""), request.note.value_or("")};
  if (update.state == kOpen) {
    if (it->declined_by && *it->declined_by == peer) {
      throw RequestRefused("job " + id + " was already declined by " + peer);
    }
    ++it->declines;
    it->declined_by = peer;
  }

  Move(*it, update, NowStamp());
  if (!update.note.empty()) it->note = update.note;
  Record(update.state.empty() ? std::string("note") : update.state, peer, id, update.note);

  ActionResult result{*it, std::nullopt};
  if (it->Closed()) result.outcome = JobOutcome{it->id, JobStateName(it->state), it->result};

  for (auto& presence : peers_) {
    if (presence.name != peer) continue;
    presence.state = JobStateName(it->state);
    presence.job = it->id;
  }
  return result;
}

std::optional<Job> Board::Find(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
  if (it == jobs_.end()) return std::nullopt;
  return *it;
}

json::value Board::Snapshot(std::chrono::seconds offline_after) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string now = NowStamp();

  json::array jobs;
  for (const auto& job : jobs_) jobs.push_back(Render(job, now));

  json::array peers;
  for (const auto& presence : peers_) {
    json::object entry;
    entry["name"] = presence.name;
    entry["last_seen"] = presence.last_seen;
    entry["online"] = AgeSeconds(presence.last_seen, now) <= offline_after.count();
    entry["state"] = presence.state;
    if (!presence.job.empty()) entry["job"] = presence.job;
    peers.push_back(std::move(entry));
  }

  json::array audit;
  for (const auto& entry : audit_) {
    audit.push_back(json::object{{"at", entry.at},
                                 {"kind", entry.kind},
                                 {"peer", entry.peer},
                                 {"job", entry.job},
                                 {"text", entry.text}});
  }

  json::object out;
  out["success"] = true;
  out["jobs"] = std::move(jobs);
  out["peers"] = std::move(peers);
  out["audit"] = std::move(audit);
  return out;
}

}  // namespace pu::serve
