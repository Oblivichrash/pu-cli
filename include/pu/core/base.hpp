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

}  // namespace pu
