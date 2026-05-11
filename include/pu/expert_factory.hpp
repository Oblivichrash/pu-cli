// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "pu/expert.hpp"
#include "pu/backend.hpp"
#include "pu/expert_config.hpp"

#include <memory>
#include <unordered_map>

namespace pu::expert {

class ExpertFactory {
 public:
  virtual ~ExpertFactory() = default;
  virtual std::unique_ptr<BaseExpert> Create(
      const config::ExpertEntry& entry,
      std::unique_ptr<backend::Backend> backend) = 0;
};

class ExpertRegistry {
 public:
  static ExpertRegistry& Instance();

  void RegisterFactory(config::ExpertType type, std::unique_ptr<ExpertFactory> factory);
  std::unique_ptr<BaseExpert> CreateExpert(const config::ExpertEntry& entry);

 private:
  ExpertRegistry() = default;
  std::unordered_map<config::ExpertType, std::unique_ptr<ExpertFactory>> factories_;
};

void RegisterBuiltinFactories();

}  // namespace pu::expert
