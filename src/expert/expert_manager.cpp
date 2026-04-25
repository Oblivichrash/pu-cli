// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert.hpp"
#include "pu/backend.hpp"

#include <iostream>
#include <sstream>

namespace pu::expert {

ExpertManager::ExpertManager(std::unique_ptr<backend::Backend> router)
    : router_(std::move(router)) {}

void ExpertManager::RegisterExpert(std::unique_ptr<BaseExpert> expert) {
  if (!expert) return;
  std::string name = expert->Name();
  if (experts_.count(name)) {
    std::cerr << "[ExpertManager] Duplicate expert name: " << name << "\n";
    return;
  }
  experts_[name] = std::move(expert);
}

void ExpertManager::SetActiveExpert(const std::string& name) {
  if (experts_.count(name)) {
    active_expert_ = name;
  }
}

std::string ExpertManager::GetActiveExpert() const {
  return active_expert_;
}

std::string ExpertManager::Dispatch(const std::string& input) {
  if (experts_.empty()) {
    return "No experts available.";
  }

  std::string target = RouteToExpert(input);
  auto it = experts_.find(target);
  if (it == experts_.end()) {
    it = experts_.find("chat");
    if (it == experts_.end()) {
      it = experts_.begin();
    }
  }

  active_expert_ = it->first;

  ExpertContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallExpert(name, inp);
  };
  ctx.request_confirmation = [](const std::string& prompt) {
    std::cout << "[CONFIRM] " << prompt << " [y/N] ";
    std::string answer;
    std::getline(std::cin, answer);
    return (answer == "y" || answer == "Y");
  };
  ctx.working_dir = ".";

  std::cout << "\n[" << active_expert_ << "] " << std::flush;

  return it->second->Handle(input, ctx);
}

std::string ExpertManager::CallExpert(const std::string& expert_name, const std::string& input) {
  auto it = experts_.find(expert_name);
  if (it == experts_.end()) {
    return "Expert not found: " + expert_name;
  }

  ExpertContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallExpert(name, inp);
  };
  ctx.working_dir = ".";

  return it->second->Handle(input, ctx);
}

std::string ExpertManager::RouteToExpert(const std::string& input) {
  if (experts_.size() == 1) {
    return experts_.begin()->first;
  }

  std::ostringstream prompt;
  prompt << "You are a strict router. Direct the user request to the best expert.\n"
         << "Available experts:\n";
  for (const auto& [name, expert] : experts_) {
    prompt << "- " << name << ": " << expert->Description() << "\n";
  }
  prompt << "\nRules:\n"
         << "- Use 'chat' for conversation, questions, explanations.\n";
  if (experts_.count("bash")) {
    prompt << "- Use 'bash' ONLY when the user explicitly asks to execute a command.\n";
  }
  prompt << "- If the user says 'ask <expert>' or 'switch to <expert>', "
         << "immediately route to that expert.\n"
         << "- If unsure, default to 'chat'.\n"
         << "- Output ONLY the expert name, no extra text.\n"
         << "\nExample:\n"
         << "User request: \"Ask bash to list files\"\n"
         << "Expert name: bash\n"
         << "\nUser request: \"" << input << "\"\n"
         << "Expert name:";

  std::vector<backend::Message> history;
  history.push_back({backend::Message::Role::kUser, prompt.str()});

  std::string selected = "chat";
  try {
    bool first = true;
    router_->Chat(history, [&](backend::TokenType type,
                               std::string_view token,
                               bool is_final) {
      if (is_final) return;
      if (type == backend::TokenType::kContent) {
        if (first) {
          selected.clear();
          first = false;
        }
        selected.append(token);
      }
    });
  } catch (const std::exception& e) {
    std::cerr << "[Router] LLM call failed, falling back to 'chat': " << e.what() << "\n";
    return "chat";
  }

  selected.erase(0, selected.find_first_not_of(" \t\n\r"));
  selected.erase(selected.find_last_not_of(" \t\n\r") + 1);

  if (experts_.find(selected) == experts_.end()) {
    std::cerr << "[Router] Unexpected expert name '" << selected << "', falling back to 'chat'\n";
    return "chat";
  }

  return selected;
}

void ExpertManager::ClearSessions() {
  for (auto& [name, expert] : experts_) {
    expert->ResetSession();
  }
  active_expert_.clear();
}

backend::Backend& ExpertManager::GetRouterBackend() {
  return *router_;
}

}  // namespace pu::expert
