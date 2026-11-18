// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace pu {

using CancelToken = std::shared_ptr<std::atomic<bool>>;

class RuntimeError : public std::runtime_error {
 public:
  explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

class RequestRefused : public RuntimeError {
 public:
  using RuntimeError::RuntimeError;
};

class NotFound : public RuntimeError {
 public:
  using RuntimeError::RuntimeError;
};

class GenericError : public RuntimeError {
 public:
  using RuntimeError::RuntimeError;
};

// A request the transport carried and a gateway refused, or one that never completed. The
// status is kept because a caller's next move depends on it: 429 and 5xx are worth asking
// again, 4xx is not, and the message alone cannot tell them apart without parsing prose.
class HttpError : public GenericError {
 public:
  explicit HttpError(const std::string& msg, unsigned status = 0)
      : GenericError(msg), status_(status) {}

  unsigned status() const { return status_; }

  // Whether asking again could plausibly answer differently. A rate limit, an overloaded
  // gateway and a server fault are all transient; a malformed request is not, and repeating
  // it only spends the same time twice.
  bool IsTransient() const {
    if (transient_) return true;
    return status_ == 429 || status_ == 408 || status_ >= 500;
  }

  // A request that never reached a status at all — a refused connection, a reset socket — is
  // transient when the client says so, since no status exists to classify it by.
  void MarkTransient() { transient_ = true; }

 private:
  unsigned status_ = 0;
  bool transient_ = false;
};

namespace uuid {

inline std::string Generate() {
  static thread_local std::mt19937 generator(std::random_device{}());
  static thread_local std::uniform_int_distribution<int> nibble(0, 15);
  constexpr char kHex[] = "0123456789abcdef";

  std::string id(36, '-');
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    id[i] = kHex[nibble(generator)];
  }
  id[14] = '4';
  id[19] = kHex[(nibble(generator) & 0x3) | 0x8];
  return id;
}

}  // namespace uuid

}  // namespace pu
