// SPDX-License-Identifier: GPL-3.0-only
#include "pu/renderer.hpp"

#include "infra/platform.hpp"

#include <iostream>

namespace pu {

void SetupSignalHandler() { pu::platform::SetupSignalHandler(); }
bool IsInterrupted() { return pu::platform::IsInterrupted(); }
void ClearInterruptFlag() { pu::platform::ClearInterruptFlag(); }

backend::ChatCallback StreamingRenderer::Create(bool show_reasoning) {
  bool first_reasoning = true;
  return [show_reasoning, first_reasoning](backend::TokenType type, std::string_view token,
                                           bool is_final) mutable {
    OnToken(type, token, is_final, show_reasoning, first_reasoning);
  };
}

void StreamingRenderer::OnToken(backend::TokenType type, std::string_view token, bool is_final,
                                bool show_reasoning, bool& first_reasoning) {
  if (type == backend::TokenType::kReasoning && show_reasoning) {
    if (first_reasoning) { std::cerr << "[Thinking] "; first_reasoning = false; }
    std::cerr << token << std::flush;
    if (is_final) std::cerr << '\n';
    return;
  }
  if (type == backend::TokenType::kContent) {
    std::cout << token << std::flush;
    if (is_final) std::cout << '\n';
  }
}

}  // namespace pu
