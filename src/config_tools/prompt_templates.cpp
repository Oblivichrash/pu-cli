// SPDX-License-Identifier: GPL-3.0-only
#include "prompt_templates.hpp"

namespace pu::config_tools {

const std::map<std::string, std::string>& GetPromptTemplates() {
  static const std::map<std::string, std::string> templates = {
      {"general",
       "You are a helpful AI assistant. Answer the user's questions accurately and concisely."},
      {"code-review",
       "You are a senior code reviewer. Analyze the provided code for correctness, "
       "security, performance, and style issues. Provide specific, actionable feedback."},
      {"documentation",
       "You are a technical documentation writer. Produce clear, well-structured "
       "documentation with examples where appropriate."},
      {"ops",
       "You are a DevOps engineer. Help with system administration, deployment, "
       "monitoring, and infrastructure tasks. Prioritize safety and best practices."},
  };
  return templates;
}

}  // namespace pu::config_tools