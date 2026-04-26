// SPDX-License-Identifier: GPL-3.0-only
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

struct ExpertContext {
  std::function<std::string(const std::string& name, const std::string& input)> call_expert;
  std::function<bool(const std::string& message)> request_confirmation;
  std::string working_dir;
  bool show_reasoning = false;
};

class BaseExpert {
 public:
  virtual ~BaseExpert() = default;
  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual std::string Handle(const std::string& input, ExpertContext& ctx) = 0;
  virtual void ResetSession() = 0;
};

class ExpertManager {
 public:
  explicit ExpertManager(std::unique_ptr<backend::Backend> router);
  void RegisterExpert(std::unique_ptr<BaseExpert> expert);
  std::string Dispatch(const std::string& input);
  std::string CallExpert(const std::string& expert_name, const std::string& input);
  void ClearSessions();
  void SetActiveExpert(const std::string& name);
  std::string GetActiveExpert() const;
  backend::Backend& GetRouterBackend();
  void SetShowReasoning(bool enable);

 private:
  std::string RouteToExpert(const std::string& input);
  std::unique_ptr<backend::Backend> router_;
  std::unordered_map<std::string, std::unique_ptr<BaseExpert>> experts_;
  std::string active_expert_;
  bool show_reasoning_ = false;
};

}  // namespace pu::expert
