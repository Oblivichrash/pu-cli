// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/json.hpp"
#include "tests/mocks/peer_harness.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <boost/json.hpp>

#include <string>

using pu::json::ValueOrDefault;
using pu::tests::HttpResponse;
using pu::tests::PeerServer;
using pu::tests::TestHttpClient;

namespace {

boost::json::object Post(const std::string& to, const std::string& intent) {
  return boost::json::object{{"from", "A"}, {"to", to}, {"intent", intent}};
}

const boost::json::object* Job(const HttpResponse& res, const std::string& id) {
  static boost::json::object found;
  for (const auto& job : boost::json::parse(res.body).as_object().at("jobs").as_array()) {
    if (ValueOrDefault<std::string>(job, "id", "") == id) {
      found = job.as_object();
      return &found;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("a posted job is readable from the board") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  CHECK(created.status == 200);

  const boost::json::value body = boost::json::parse(created.body);
  CHECK(ValueOrDefault<bool>(body, "success", false));

  const std::string id = ValueOrDefault<std::string>(body.at("job"), "id", "");
  REQUIRE_FALSE(id.empty());
  CHECK(ValueOrDefault<std::string>(body.at("job"), "state", "") == "open");
  CHECK(ValueOrDefault<std::string>(body.at("job"), "to", "") == "B");

  const HttpResponse listed = client.GetRaw("/api/jobs");
  CHECK(listed.status == 200);
  const auto* job = Job(listed, id);
  REQUIRE(job != nullptr);
  CHECK(ValueOrDefault<std::string>(*job, "intent", "") == "check the build");
}

TEST_CASE("the holder moves a job the board answers with when it closes") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");
  REQUIRE_FALSE(id.empty());

  const HttpResponse working = client.Patch(
      "/api/jobs",
      boost::json::object{{"job", id}, {"from", "B"}, {"state", "working"}});
  CHECK(working.status == 200);
  CHECK(ValueOrDefault<std::string>(boost::json::parse(working.body).at("job"), "state", "") ==
        "working");

  const HttpResponse done = client.Patch(
      "/api/jobs",
      boost::json::object{{"job", id}, {"from", "B"}, {"state", "done"}, {"result", "looks fine"}});
  CHECK(done.status == 200);

  const boost::json::value body = boost::json::parse(done.body);
  REQUIRE(body.as_object().contains("outcome"));
  CHECK(ValueOrDefault<std::string>(body.at("outcome"), "state", "") == "done");
  CHECK(ValueOrDefault<std::string>(body.at("outcome"), "result", "") == "looks fine");
}

TEST_CASE("a job that is only offered can be declined and reopened") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  const HttpResponse declined = client.Patch(
      "/api/jobs",
      boost::json::object{{"job", id}, {"from", "B"}, {"state", "open"}, {"note", "not mine"}});
  CHECK(declined.status == 200);

  const boost::json::value body = boost::json::parse(declined.body).at("job");
  CHECK(ValueOrDefault<std::string>(body, "state", "") == "open");
  CHECK(ValueOrDefault<std::string>(body, "declined_by", "") == "B");
  CHECK(ValueOrDefault<int>(body, "declines", 0) == 1);
}

// Reading is being heard from: the pull carries the token, so the name it records is the one the
// token speaks for and nobody can claim a liveness they cannot prove.
TEST_CASE("a board read records the peer that made it") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse read = client.GetRaw("/api/jobs");
  CHECK(read.status == 200);

  const boost::json::value body = boost::json::parse(read.body);
  REQUIRE(body.as_object().contains("peers"));
  bool seen = false;
  for (const auto& peer : body.at("peers").as_array()) {
    if (ValueOrDefault<std::string>(peer, "name", "") != "A") continue;
    seen = true;
    CHECK(ValueOrDefault<bool>(peer, "online", false));
  }
  CHECK(seen);
  // A read answers with the board, so a caller learns what it holds in the same round trip.
  CHECK(body.as_object().contains("jobs"));
}

TEST_CASE("a board request without the token is refused") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto anonymous = server.AnonymousClient();

  CHECK(anonymous.GetRaw("/api/jobs").status == 403);
  CHECK(anonymous
            .PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")))
            .status == 403);
}

TEST_CASE("an unreadable request is answered with the status that names whose it is") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse no_target =
      client.PostRaw("/api/jobs", boost::json::serialize(boost::json::object{{"intent", "x"}}));
  CHECK(no_target.status == 400);
  CHECK(ValueOrDefault<bool>(boost::json::parse(no_target.body), "success", true) == false);

  const HttpResponse unknown_job = client.Patch(
      "/api/jobs",
      boost::json::object{{"job", "no-such-job"}, {"from", "B"}, {"state", "claimed"}});
  CHECK(unknown_job.status == 404);

}

TEST_CASE("claiming a job reports the claim itself, not an outcome") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  const HttpResponse claimed = client.Patch(
      "/api/jobs", boost::json::object{{"job", id}, {"from", "B"}, {"state", "claimed"}});
  CHECK(claimed.status == 200);

  const boost::json::value body = boost::json::parse(claimed.body);
  CHECK(ValueOrDefault<std::string>(body.at("job"), "state", "") == "claimed");
  CHECK_FALSE(body.as_object().contains("outcome"));
}

TEST_CASE("a job belongs to the peer it names, so nobody else can move it") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  const HttpResponse stranger = client.Patch(
      "/api/jobs", boost::json::object{{"job", id}, {"from", "C"}, {"state", "claimed"}});
  CHECK(stranger.status == 400);
  CHECK(ValueOrDefault<bool>(boost::json::parse(stranger.body), "success", true) == false);

  const HttpResponse listed = client.GetRaw("/api/jobs");
  const auto* job = Job(listed, id);
  REQUIRE(job != nullptr);
  CHECK(ValueOrDefault<std::string>(*job, "state", "") == "open");
}

TEST_CASE("a person answers a blocked job, and it goes back to work") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  client.Patch("/api/jobs",
               boost::json::object{{"job", id},
                                   {"from", "B"},
                                   {"state", "blocked"},
                                   {"question", "which branch?"}});

  const HttpResponse answered = client.PostRaw(
      "/api/jobs/answer", boost::json::serialize(boost::json::object{
                              {"job", id}, {"answer", "the release one"}}));
  CHECK(answered.status == 200);

  const boost::json::value body = boost::json::parse(answered.body);
  CHECK(ValueOrDefault<std::string>(body.at("job"), "state", "") == "working");
  CHECK(ValueOrDefault<std::string>(body.at("job"), "note", "") == "the release one");
  CHECK_FALSE(body.at("job").as_object().contains("question"));
}

TEST_CASE("an answer is refused when there is no question to answer") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  const HttpResponse too_early = client.PostRaw(
      "/api/jobs/answer", boost::json::serialize(boost::json::object{
                              {"job", id}, {"answer", "the release one"}}));
  CHECK(too_early.status == 400);

  const HttpResponse unknown = client.PostRaw(
      "/api/jobs/answer", boost::json::serialize(boost::json::object{
                              {"job", "no-such-job"}, {"answer", "anything"}}));
  CHECK(unknown.status == 404);

  const HttpResponse empty = client.PostRaw(
      "/api/jobs/answer", boost::json::serialize(boost::json::object{{"job", id}}));
  CHECK(empty.status == 400);
}

TEST_CASE("a person asks what is happening and takes the job back, both without naming a peer") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto client = server.Client();

  const HttpResponse created =
      client.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");
  client.Patch("/api/jobs", boost::json::object{{"job", id}, {"from", "B"}, {"state", "working"}});

  const HttpResponse chased = client.PostRaw(
      "/api/jobs/chase", boost::json::serialize(boost::json::object{
                             {"job", id}, {"note", "which branch is it on?"}}));
  CHECK(chased.status == 200);
  const boost::json::value asked = boost::json::parse(chased.body);
  CHECK(ValueOrDefault<std::string>(asked.at("job"), "state", "") == "working");
  CHECK(ValueOrDefault<int>(asked.at("job"), "chases", 0) == 1);

  const HttpResponse dropped = client.PostRaw(
      "/api/jobs/drop", boost::json::serialize(boost::json::object{
                            {"job", id}, {"note", "the branch is gone"}}));
  CHECK(dropped.status == 200);
  const boost::json::value closed = boost::json::parse(dropped.body);
  CHECK(ValueOrDefault<std::string>(closed.at("job"), "state", "") == "cancelled");
  CHECK(ValueOrDefault<std::string>(closed.at("outcome"), "state", "") == "cancelled");

  const HttpResponse unknown = client.PostRaw(
      "/api/jobs/drop", boost::json::serialize(boost::json::object{
                            {"job", "no-such-job"}, {"note", "why not"}}));
  CHECK(unknown.status == 404);

  const HttpResponse blank = client.PostRaw(
      "/api/jobs/chase", boost::json::serialize(boost::json::object{{"job", id}}));
  CHECK(blank.status == 400);
}

TEST_CASE("the page reads the board without a token, and reads no peers with it") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();
  auto page = server.AnonymousClient();

  const HttpResponse empty = page.GetRaw("/api/board/local");
  CHECK(empty.status == 200);
  CHECK(boost::json::parse(empty.body).as_object().contains("jobs"));

  auto peer = server.Client();
  const HttpResponse created =
      peer.PostRaw("/api/jobs", boost::json::serialize(Post("B", "check the build")));
  REQUIRE(created.status == 200);
  const std::string id =
      ValueOrDefault<std::string>(boost::json::parse(created.body).at("job"), "id", "");

  const HttpResponse read = page.GetRaw("/api/board/local");
  CHECK(read.status == 200);
  const boost::json::object body = boost::json::parse(read.body).as_object();
  REQUIRE(body.at("jobs").as_array().size() == 1);
  CHECK(ValueOrDefault<std::string>(body.at("jobs").as_array().at(0), "id", "") == id);
  CHECK_FALSE(body.contains("peers"));
}

TEST_CASE("a peer's read of the board still presents the token") {
  PeerServer server("A", boost::json::object{{"content", "hi"}}, "stop");
  server.Start();

  const HttpResponse anonymous = server.AnonymousClient().GetRaw("/api/jobs");
  CHECK(anonymous.status == 403);
}
