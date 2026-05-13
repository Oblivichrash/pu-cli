// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include <algorithm>
#include <optional>
#include <vector>

namespace pu::backend {

inline std::vector<Message> InjectSystemPrompt(
    const std::vector<Message>& history,
    const std::optional<std::string>& system_prompt) {
  if (!system_prompt) return history;
  if (std::any_of(history.begin(), history.end(), [](const Message& m) {
        return m.role == Message::Role::kSystem;
      })) {
    return history;
  }
  std::vector<Message> messages;
  messages.reserve(history.size() + 1);
  messages.emplace_back(Message::Role::kSystem, *system_prompt);
  messages.insert(messages.end(), history.begin(), history.end());
  return messages;
}

}  // namespace pu::backend
