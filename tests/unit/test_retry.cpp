// SPDX-License-Identifier: GPL-3.0-only

#include "pu/core/retry.hpp"

#include "pu/core/base.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace pu;
using namespace pu::core;
using namespace std::chrono_literals;

namespace {

// A policy with no waiting, so a test about decisions does not spend the wall clock proving
// it: the delays are still computed and still handed to the notice callback.
RetryPolicy ImmediatePolicy(int attempts = 3) {
  RetryPolicy policy;
  policy.max_attempts = attempts;
  policy.initial_delay = 0ms;
  policy.max_delay = 0ms;
  return policy;
}

}  // namespace

TEST_CASE("IsTransientStatus names the statuses worth repeating", "[retry]") {
  // The four the trade calls worth another try together with the two that are the caller's
  // fault and would be repeated to the same answer.
  REQUIRE(IsTransientStatus(429));
  REQUIRE(IsTransientStatus(408));
  REQUIRE(IsTransientStatus(500));
  REQUIRE(IsTransientStatus(502));
  REQUIRE(IsTransientStatus(503));
  REQUIRE(IsTransientStatus(504));

  REQUIRE_FALSE(IsTransientStatus(400));
  REQUIRE_FALSE(IsTransientStatus(401));
  REQUIRE_FALSE(IsTransientStatus(403));
  REQUIRE_FALSE(IsTransientStatus(404));
  REQUIRE_FALSE(IsTransientStatus(200));
  // A status of zero is a failure that never got one, which is a transport failure and not
  // something this function is asked about.
  REQUIRE_FALSE(IsTransientStatus(0));
}

TEST_CASE("DelayBeforeNextAttempt backs off exponentially until the ceiling", "[retry]") {
  RetryPolicy policy;
  policy.initial_delay = 500ms;
  policy.max_delay = 4000ms;

  // The wait before a repeat doubles with the attempt number, so the second attempt waits the
  // initial delay and each one after it waits twice the last.
  REQUIRE(DelayBeforeNextAttempt(policy, 0) == 500ms);
  REQUIRE(DelayBeforeNextAttempt(policy, 1) == 1000ms);
  REQUIRE(DelayBeforeNextAttempt(policy, 2) == 2000ms);
  REQUIRE(DelayBeforeNextAttempt(policy, 3) == 4000ms);
  // Past the ceiling it stays at the ceiling rather than growing without bound.
  REQUIRE(DelayBeforeNextAttempt(policy, 4) == 4000ms);
  REQUIRE(DelayBeforeNextAttempt(policy, 20) == 4000ms);
}

TEST_CASE("WithRetry repeats a transient failure and returns once it succeeds", "[retry]") {
  int attempts = 0;
  std::vector<int> noticed;

  WithRetry(
      ImmediatePolicy(), [&](int attempt, const std::string&, std::chrono::milliseconds) {
        noticed.push_back(attempt);
      },
      [&] {
        ++attempts;
        if (attempts < 3) throw HttpError("service unavailable", 503);
      });

  REQUIRE(attempts == 3);
  // The notice is told about the two failures, not about the attempt that worked.
  REQUIRE(noticed == std::vector<int>{1, 2});
}

TEST_CASE("WithRetry rethrows a failure that is not transient", "[retry]") {
  int attempts = 0;

  // A 400 is the same request answered the same way, so it is raised on the first try rather
  // than spent three times.
  REQUIRE_THROWS_AS(
      WithRetry(ImmediatePolicy(), nullptr,
                [&] {
                  ++attempts;
                  throw HttpError("bad request", 400);
                }),
      HttpError);
  REQUIRE(attempts == 1);
}

TEST_CASE("WithRetry raises the last failure once its attempts run out", "[retry]") {
  int attempts = 0;

  // The error a caller sees is the one the last attempt raised, not a wrapper saying it gave
  // up: the status and the message are the provider's own.
  try {
    WithRetry(ImmediatePolicy(3), nullptr,
              [&] {
                ++attempts;
                throw HttpError("still unavailable", 503);
              });
    FAIL("expected the failure to be raised");
  } catch (const HttpError& e) {
    REQUIRE(e.status() == 503);
    REQUIRE(e.IsTransient());
  }
  REQUIRE(attempts == 3);
}

TEST_CASE("WithRetry passes a failure it does not own straight through", "[retry]") {
  // The policy is about transport statuses, so a failure that is not one is not retried and
  // not swallowed: a configuration error raised inside the attempt is still that error.
  REQUIRE_THROWS_AS(
      WithRetry(ImmediatePolicy(), nullptr, [] { throw GenericError("nope"); }), GenericError);
}

TEST_CASE("WithRetry returns normally when the first attempt does", "[retry]") {
  int attempts = 0;
  WithRetry(
      ImmediatePolicy(), [&](int, const std::string&, std::chrono::milliseconds) { FAIL("retried"); },
      [&] { ++attempts; });
  REQUIRE(attempts == 1);
}
