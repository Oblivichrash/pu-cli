// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The pieces every layer above core is allowed to assume: a shared cancel flag,
// the error hierarchy, identifier generation, and the data directory.

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace pu {

// Shared cancel flag observed by every layer (LLM providers and the HTTP
// client), so it lives in the base layer rather than next to the HTTP adapter.
using CancelToken = std::shared_ptr<std::atomic<bool>>;

// Base class for all non-recoverable runtime errors. Catchers at the top level
// can catch this (or std::exception) to produce a friendly error message
// without crashing.
class RuntimeError : public std::runtime_error {
public:
  explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

// General-purpose error, e.g. configuration parsing.
class Error : public RuntimeError {
public:
  using RuntimeError::RuntimeError;
};

// HTTP and network errors, raised by the HTTP client.
class HttpError : public Error {
public:
  explicit HttpError(const std::string& msg) : Error(msg) {}
};

namespace uuid {

// RFC 4122 version 4, lowercase, hyphenated (8-4-4-4-12).
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

// The data directory: PU_HOME when set, otherwise the project's .pu directory.
inline std::filesystem::path GetDataDir() {
  if (const char* env = std::getenv("PU_HOME")) {
    return std::filesystem::path(env);
  }

  return std::filesystem::current_path() / ".pu";
}

}  // namespace path

}  // namespace pu
