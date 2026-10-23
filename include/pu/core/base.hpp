// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

class HttpError : public GenericError {
 public:
  explicit HttpError(const std::string& msg) : GenericError(msg) {}
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

namespace path {

// The one directory PU_HOME moves, and the only thing it moves: logs. Session and
// agent state stay under the workspace's own .pu/.
//
// A process with no home of its own finds a directory named after it instead: the cases of one
// test binary run at the same time, and one shared log directory is a file several processes
// rotate at once, which renames the sink out from under a writer and ends a case with a signal
// rather than a failed assertion.
inline std::filesystem::path GetLogDir() {
  if (const char* env = std::getenv("PU_HOME")) {
    return std::filesystem::path(env) / "logs";
  }

#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = static_cast<int>(getpid());
#endif
  return std::filesystem::current_path() / ".pu" / ("logs-" + std::to_string(pid));
}

}  // namespace path

}  // namespace pu
