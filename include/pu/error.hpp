// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {

class HttpError : public std::runtime_error {
 public:
  explicit HttpError(const std::string& msg) : std::runtime_error(msg), detail_(msg) {}
  explicit HttpError(const std::string& msg, const std::string& detail)
      : std::runtime_error(msg), detail_(detail) {}
  const std::string& detail() const { return detail_; }

 private:
  std::string detail_;
};

class StoreError : public std::runtime_error {
 public:
  explicit StoreError(const std::string& msg) : std::runtime_error(msg) {}
};

}  // namespace pu
