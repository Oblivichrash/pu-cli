// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/delegation.hpp"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace pu::core {

std::string Delegation::GenerateId() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d%H%M%S");

  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(1000, 9999);
  ss << "-" << dis(gen);

  return "deleg-" + ss.str();
}

bool Delegation::IsTimeout() const {
  if (deadline.time_since_epoch().count() == 0) return false;
  return std::chrono::steady_clock::now() > deadline;
}

}  // namespace pu::core