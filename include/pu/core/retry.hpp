// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// Asking again, when asking again could answer differently.
//
// A model endpoint answers 429 and 5xx as a matter of course, and neither says the request was
// wrong. The policy here is the part that decides whether a failure is worth a second attempt
// and how long to wait before it, kept apart from the attempt itself so a test can pin the
// decision without a socket or a clock.
//
// What it does not do is decide what a retry means for a caller's state: a stream that failed
// after emitting half a reply has already shown that half to whoever was watching, and only
// the caller knows whether that is recoverable. The policy answers "ask again?" and nothing
// else.
//
// The wait is exponential and nothing else. A gateway's `Retry-After` is not read: no caller
// holds a response header at the moment it decides to wait, and the hint would have to be
// carried out of a stream callback that has already returned. A comment promising to honour
// it is worse than not mentioning it.
//

#include <chrono>
#include <functional>
#include <string>

namespace pu::core {

// How many times a turn is attempted in total, first attempt included, and how long the waits
// between them grow.
struct RetryPolicy {
  // The number of attempts, not of retries: `3` means the first try and two more.
  int max_attempts = 3;
  // The first wait. Each further one doubles it, up to `max_delay`.
  std::chrono::milliseconds initial_delay{500};
  std::chrono::milliseconds max_delay{8000};
};

// The wait before attempt `attempt + 1`, where `attempt` counts from zero. The growth is
// exponential by design: a gateway under load is not helped by every client retrying at the
// same cadence.
std::chrono::milliseconds DelayBeforeNextAttempt(const RetryPolicy& policy, int attempt);

// Runs `attempt` until it succeeds, the policy's attempts run out, or it throws something a
// retry cannot answer. Any exception that is not worth repeating is rethrown on the spot, so a
// caller's error handling keeps seeing the failure it would have seen without this layer.
//
// `on_retry` is told what failed and how long the wait will be, once per retry, so a caller can
// say so without the policy knowing how to print.
using RetryNotice = std::function<void(int attempt, const std::string& why,
                                       std::chrono::milliseconds delay)>;

void WithRetry(const RetryPolicy& policy, const RetryNotice& on_retry,
               const std::function<void()>& attempt);

// Whether a failure is one that asking again could answer differently. `status` is the HTTP
// code, or zero when the failure never reached one — a refused connection, a reset socket,
// which are transient for the same reason a 503 is.
bool IsTransientStatus(unsigned status);

}  // namespace pu::core
