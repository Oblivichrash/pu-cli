// SPDX-License-Identifier: GPL-3.0-only

#include "pu/serve/board.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace pu;
using serve::Board;
using serve::Job;
using serve::JobRequest;
using serve::JobState;

namespace {

JobRequest Post(const std::string& to, const std::string& intent) {
  JobRequest request;
  request.from = "A";
  request.to = to;
  request.intent = intent;
  return request;
}

JobRequest Move(const std::string& job, const std::string& by, const std::string& state) {
  JobRequest request;
  request.job = job;
  request.from = by;
  request.state = state;
  return request;
}

const boost::json::object* Find(const boost::json::value& snapshot, const std::string& id) {
  for (const auto& job : snapshot.as_object().at("jobs").as_array()) {
    if (json::ValueOrDefault<std::string>(job, "id", "") == id) return &job.as_object();
  }
  return nullptr;
}

const boost::json::object* Presence(const boost::json::value& snapshot, const std::string& name) {
  for (const auto& peer : snapshot.as_object().at("peers").as_array()) {
    if (json::ValueOrDefault<std::string>(peer, "name", "") == name) return &peer.as_object();
  }
  return nullptr;
}

}  // namespace

TEST_CASE("a posted job starts open and carries the peer it is for") {
  Board board;
  const auto posted = board.Post(Post("B", "summarise the changelog"));

  CHECK(posted.job.state == JobState::kOpen);
  CHECK(posted.job.to == "B");
  CHECK(posted.job.from == "A");
  CHECK(posted.job.intent == "summarise the changelog");
  CHECK(posted.job.due_at == std::nullopt);
  CHECK(posted.job.created_at == posted.job.updated_at);

  const auto snapshot = board.Snapshot(std::chrono::seconds(45));
  const auto* job = Find(snapshot, posted.job.id);
  REQUIRE(job != nullptr);
  CHECK(json::ValueOrDefault<std::string>(*job, "state", "") == "open");
  CHECK(json::ValueOrDefault<bool>(*job, "overdue", true) == false);
}

TEST_CASE("a job needs a peer and an intent to exist") {
  Board board;

  JobRequest no_target = Post("", "do it");
  CHECK_THROWS_AS(board.Post(no_target), RequestRefused);

  JobRequest no_intent = Post("B", "");
  CHECK_THROWS_AS(board.Post(no_intent), RequestRefused);
}

TEST_CASE("a duration sets the deadline and an unreadable one is refused") {
  Board board;

  auto request = Post("B", "look at the logs");
  request.due = "30m";
  CHECK(board.Post(request).job.due_at.has_value());

  request.due = "half an hour";
  CHECK_THROWS_AS(board.Post(request), RequestRefused);
}

TEST_CASE("the holder walks the job through to done and the result comes back") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;

  CHECK(board.Update(Move(id, "B", "claimed")).job.state == JobState::kClaimed);
  CHECK(board.Update(Move(id, "B", "working")).job.state == JobState::kWorking);

  auto finish = Move(id, "B", "done");
  finish.result = "everything green";
  const auto closed = board.Update(finish);

  CHECK(closed.job.state == JobState::kDone);
  REQUIRE(closed.outcome.has_value());
  CHECK(closed.outcome->state == "done");
  REQUIRE(closed.outcome->result.has_value());
  CHECK(*closed.outcome->result == "everything green");
}

TEST_CASE("only the peer a job is for can move it") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;

  CHECK_THROWS_AS(board.Update(Move(id, "C", "claimed")), RequestRefused);
  CHECK_THROWS_AS(board.Update(Move(id, "A", "done")), RequestRefused);
}

TEST_CASE("an ended job cannot be moved again") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  board.Update(Move(id, "B", "done"));

  CHECK_THROWS_AS(board.Update(Move(id, "B", "working")), RequestRefused);
}

TEST_CASE("a blocked job must say what it is asking") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;

  CHECK_THROWS_AS(board.Update(Move(id, "B", "blocked")), RequestRefused);

  auto ask = Move(id, "B", "blocked");
  ask.question = "which branch should I check?";
  const auto held = board.Update(ask);

  CHECK(held.job.state == JobState::kBlocked);
  REQUIRE(held.job.question.has_value());
  CHECK(*held.job.question == "which branch should I check?");
}

TEST_CASE("a decline reopens the job and records who declined it") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  board.Update(Move(id, "B", "claimed"));

  const auto declined = board.Update(Move(id, "B", "open"));

  CHECK(declined.job.state == JobState::kOpen);
  CHECK(declined.job.declines == 1);
  REQUIRE(declined.job.declined_by.has_value());
  CHECK(*declined.job.declined_by == "B");
  CHECK_THROWS_AS(board.Update(Move(id, "B", "open")), RequestRefused);
}

TEST_CASE("an unknown state or an unknown job is refused by name") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;

  CHECK_THROWS_AS(board.Update(Move(id, "B", "finished")), RequestRefused);
  CHECK_THROWS_AS(board.Update(Move("no-such-job", "B", "claimed")), NotFound);
}

TEST_CASE("a due date in the past marks the job overdue without touching its state") {
  Board board;

  auto request = Post("B", "look at the logs");
  request.due = "1s";
  const std::string id = board.Post(request).job.id;

  std::this_thread::sleep_for(std::chrono::milliseconds(2100));

  const auto snapshot = board.Snapshot(std::chrono::seconds(45));
  const auto* job = Find(snapshot, id);
  REQUIRE(job != nullptr);
  CHECK(json::ValueOrDefault<bool>(*job, "overdue", false));
  CHECK(json::ValueOrDefault<std::string>(*job, "state", "") == "open");
}

TEST_CASE("a peer is online while it reports and offline once it stops") {
  Board board;
  board.TouchPeer("B");

  const auto fresh = board.Snapshot(std::chrono::seconds(45));
  const auto* online = Presence(fresh, "B");
  REQUIRE(online != nullptr);
  CHECK(json::ValueOrDefault<bool>(*online, "online", false));

  const auto stale = board.Snapshot(std::chrono::seconds(-1));
  const auto* offline = Presence(stale, "B");
  REQUIRE(offline != nullptr);
  CHECK(json::ValueOrDefault<bool>(*offline, "online", true) == false);
}

TEST_CASE("a peer moving a job shows up on the board as what it is doing") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  board.TouchPeer("B");

  board.Update(Move(id, "B", "working"));

  const auto snapshot = board.Snapshot(std::chrono::seconds(45));
  const auto* presence = Presence(snapshot, "B");
  REQUIRE(presence != nullptr);
  CHECK(json::ValueOrDefault<std::string>(*presence, "state", "") == "working");
  CHECK(json::ValueOrDefault<std::string>(*presence, "job", "") == id);
}

TEST_CASE("the board stops taking new work once it holds its limit") {
  Board board;
  for (std::size_t i = 0; i < Board::kMaxOpen; ++i) {
    board.Post(Post("B", "job " + std::to_string(i)));
  }

  CHECK_THROWS_AS(board.Post(Post("B", "one too many")), RequestRefused);
}

TEST_CASE("a job renders with the derived fields a reader needs") {
  Board board;

  auto request = Post("B", "check the build");
  request.due = "2h";
  const Job job = board.Post(request).job;
  const auto rendered = Board::Render(job).as_object();

  CHECK(json::ValueOrDefault<std::string>(rendered, "state", "") == "open");
  CHECK(json::ValueOrDefault<bool>(rendered, "overdue", true) == false);
  CHECK(rendered.contains("created_at"));
  CHECK(rendered.contains("updated_at"));
  CHECK(rendered.contains("due_at"));
  CHECK(rendered.contains("idle_seconds"));
}

TEST_CASE("an answer clears the question and puts the job back to work") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  auto asking = Move(id, "B", "blocked");
  asking.question = "which branch?";
  board.Update(asking);

  const Job answered = board.Answer(id, "the release one").job;

  CHECK(answered.state == JobState::kWorking);
  CHECK(answered.note == "the release one");
  CHECK_FALSE(answered.question.has_value());
}

TEST_CASE("an answer is refused for a job that is not blocked, or already closed") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;

  CHECK_THROWS_AS(board.Answer(id, "anything"), RequestRefused);
  CHECK_THROWS_AS(board.Answer("no-such-job", "anything"), NotFound);

  board.Update(Move(id, "B", "done"));
  CHECK_THROWS_AS(board.Answer(id, "anything"), RequestRefused);
}

TEST_CASE("a digest says nothing when the board owes the server nothing") {
  Board board;

  CHECK(board.Digest("A").empty());
  CHECK(board.Digest("").empty());

  board.Post(Post("B", "check the build"));

  CHECK(board.Digest("A").empty());
}

TEST_CASE("a digest names the jobs a server holds and what it is being asked") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  auto asking = Move(id, "B", "blocked");
  asking.question = "which branch?";
  board.Update(asking);

  const std::string digest = board.Digest("B");

  CHECK(digest.find(id) != std::string::npos);
  CHECK(digest.find("check the build") != std::string::npos);
  CHECK(digest.find("from A") != std::string::npos);
  CHECK(digest.find("blocked") != std::string::npos);
  CHECK(digest.find("which branch?") != std::string::npos);
}

TEST_CASE("a digest reports the outcome of a job the server handed out") {
  Board board;
  const std::string id = board.Post(Post("B", "check the build")).job.id;
  auto answering = Move(id, "B", "done");
  answering.result = "it is green";
  board.Update(answering);

  const std::string digest = board.Digest("A");

  CHECK(digest.find(id) != std::string::npos);
  CHECK(digest.find("to B") != std::string::npos);
  CHECK(digest.find("done") != std::string::npos);
  CHECK(digest.find("it is green") != std::string::npos);
  CHECK(board.Digest("B").empty());
}

TEST_CASE("a digest marks a job as overdue only while it is still open") {
  Board board;
  auto request = Post("B", "check the build");
  request.due = "1s";
  const std::string id = board.Post(request).job.id;
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));

  CHECK(board.Digest("B").find("overdue") != std::string::npos);

  board.Update(Move(id, "B", "working"));

  CHECK(board.Digest("B").find("overdue") != std::string::npos);

  board.Update(Move(id, "B", "done"));

  CHECK(board.Digest("A").find("overdue") == std::string::npos);
}
