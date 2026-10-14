// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/model_listing.hpp"

#include "pu/config/backend.hpp"
#include "pu/llm/model_listing.hpp"

namespace pu::config {

std::unique_ptr<pu::ModelLister> FindModelListing(BackendType type) {
  switch (type) {
    case BackendType::kOllama:
      return std::make_unique<llm::OllamaModelLister>();
    case BackendType::kOpenAI:
      return std::make_unique<llm::OpenAIModelLister>();
    case BackendType::kCodeBuddy:
      return std::make_unique<llm::CodeBuddyModelLister>();
  }
  return nullptr;
}

}  // namespace pu::config
