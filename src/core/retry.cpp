// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/retry.hpp"

#include "pu/core/base.hpp"

#include <algorithm>
#include <thread>

namespace pu::core {

namespace {

// The failures a second attempt can answer: too many requests, a request timeout, and any
// server-side fault. A 400, a 401 and a 404 all describe the request rather than the moment,
// and repeating them spends the same time twice to reach the same refusal.
bool Transient(unsigned status) {
  if (status == 408 || status == 429) return true;
  return status >= 500;
}

}  // namespace

bool IsTransientStatus(unsigned status) { return Transient(status); }

std::chrono::milliseconds DelayBeforeNextAttempt(const RetryPolicy& policy, int attempt) {
  // Doubling by shift, with the ceiling reached before the shift could overflow a wide type.
  const int doublings = std::max(0, attempt);
  const long long cap = static_cast<long long>(policy.max_delay.count());
  long long delay = static_cast<long long>(policy.initial_delay.count());
  for (int i = 0; i < doublings && delay < cap; ++i) {
    delay *= 2;
  }
  return std::chrono::milliseconds{std::min(delay, cap)};
}

void WithRetry(const RetryPolicy& policy, const RetryNotice& on_retry,
               const std::function<void()>& attempt) {
  const int attempts = std::max(1, policy.max_attempts);
  for (int i = 0;; ++i) {
    try {
      attempt();
      return;
    } catch (const HttpError& e) {
      if (!e.IsTransient() || i + 1 >= attempts) throw;
      const auto delay = DelayBeforeNextAttempt(policy, i);
      if (on_retry) on_retry(i + 1, e.what(), delay);
      std::this_thread::sleep_for(delay);
    }
  }
}

}  // namespace pu::core
