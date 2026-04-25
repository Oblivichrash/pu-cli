// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// ChatExpert: conversational assistant with LLM backend.

#pragma once

#include "pu/expert.hpp"
#include "pu/backend.hpp"

#include <memory>
#include <string>

namespace pu::experts {

class ChatExpert : public pu::expert::BaseExpert {
 public:
  // 'model_id' is a human-readable label for display (e.g., model name).
  // The expert is always registered with the fixed name "chat".
  explicit ChatExpert(std::unique_ptr<pu::backend::Backend> backend,
                      const std::string& model_id = "");
  ~ChatExpert() override = default;

  std::string Name() const override { return "chat"; }
  std::string Description() const override {
    std::string desc = "General conversational assistant";
    if (!model_id_.empty()) desc += " powered by " + model_id_;
    return desc;
  }

  std::string Handle(const std::string& input, pu::expert::ExpertContext& ctx) override;
  void ResetSession() override;

 private:
  std::unique_ptr<pu::backend::Backend> backend_;
  std::vector<pu::backend::Message> history_;
  std::string model_id_;
};

}  // namespace pu::experts
