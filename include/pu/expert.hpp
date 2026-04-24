// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Expert framework base classes.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace pu::backend {
class Backend;
}  // namespace pu::backend

namespace pu::expert {

// Context passed to each expert invocation.
struct ExpertContext {
  // Call another expert by name. Returns the response string.
  std::function<std::string(const std::string& name, const std::string& input)> call_expert;

  // Request user confirmation for an action. Returns true if approved.
  std::function<bool(const std::string& message)> request_confirmation;

  // Current working directory.
  std::string working_dir;
};

// Abstract base class for all experts.
class BaseExpert {
 public:
  virtual ~BaseExpert() = default;

  // Unique identifier used for routing.
  virtual std::string Name() const = 0;

  // Human-readable description of the expert's capabilities.
  virtual std::string Description() const = 0;

  // Process a natural language input. The expert may maintain internal state
  // across multiple calls (in which case it is stateful across the session).
  virtual std::string Handle(const std::string& input, ExpertContext& ctx) = 0;

  // Reset session-level internal state (e.g., conversation history).
  virtual void ResetSession() = 0;
};

// ExpertManager: orchestrates experts and routes user requests.
class ExpertManager {
 public:
  // Takes a router backend for automatic expert selection.
  explicit ExpertManager(std::unique_ptr<backend::Backend> router);

  // Register a statically linked expert.
  void RegisterExpert(std::unique_ptr<BaseExpert> expert);

  // Process user input: route to the appropriate expert and return response.
  std::string Dispatch(const std::string& input);

  // Allow an expert to call another expert by name.
  std::string CallExpert(const std::string& expert_name, const std::string& input);

  // Clear the session of all registered experts.
  void ClearSessions();

  // Returns a pointer to the backend used for routing (e.g., for raw text generation).
  // The pointer remains valid as long as the ExpertManager exists.
  backend::Backend* GetRouterBackend();

 private:
  // Use the router LLM to select the best expert for the input.
  std::string RouteToExpert(const std::string& input);

  std::unique_ptr<backend::Backend> router_;
  std::unordered_map<std::string, std::unique_ptr<BaseExpert>> experts_;
};

}  // namespace pu::expert
