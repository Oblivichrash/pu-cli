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
  explicit ChatExpert(std::unique_ptr<pu::backend::Backend> backend,
                      const std::string& name = "chat");
  ~ChatExpert() override = default;

  std::string Name() const override { return name_; }
  std::string Description() const override {
    return "General conversational assistant powered by " + name_;
  }

  std::string Handle(const std::string& input, pu::expert::ExpertContext& ctx) override;
  void ResetSession() override;

 private:
  std::unique_ptr<pu::backend::Backend> backend_;
  std::vector<pu::backend::Message> history_;
  std::string name_;
};

}  // namespace pu::experts
