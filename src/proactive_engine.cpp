// SPDX-License-Identifier: GPL-3.0-only

#include "pu/proactive_engine.hpp"

namespace pu::expert {

void ProactiveEngine::SetEnabled(bool enabled) {
  enabled_ = enabled;
}

bool ProactiveEngine::IsEnabled() const {
  return enabled_;
}

void ProactiveEngine::SetThreshold(double threshold) {
  threshold_ = threshold;
}

double ProactiveEngine::GetThreshold() const {
  return threshold_;
}

void ProactiveEngine::SetRecentMessages(const std::vector<ChatMessage>& messages) {
  recent_messages_ = messages;
}

const std::vector<ChatMessage>& ProactiveEngine::GetRecentMessages() const {
  return recent_messages_;
}

}  // namespace pu::expert
