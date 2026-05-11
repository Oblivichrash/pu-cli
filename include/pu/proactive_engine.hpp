// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "pu/conversation.hpp"
#include <vector>

namespace pu::expert {

class ProactiveEngine {
 public:
  void SetEnabled(bool enabled);
  bool IsEnabled() const;

  void SetThreshold(double threshold);
  double GetThreshold() const;

  void SetRecentMessages(const std::vector<ChatMessage>& messages);
  const std::vector<ChatMessage>& GetRecentMessages() const;

 private:
  bool enabled_ = false;
  double threshold_ = 0.6;
  std::vector<ChatMessage> recent_messages_;
};

}  // namespace pu::expert
