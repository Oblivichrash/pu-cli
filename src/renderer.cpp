// SPDX-License-Identifier: GPL-3.0-only

#include "pu/renderer.hpp"

#include <atomic>
#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pu {

namespace {

// Global interrupt flag – shared across all active requests.
// In a single-threaded CLI this is acceptable, but future multi‑session
// support should replace it with per-request stop tokens.
std::atomic<bool> interrupted{false};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT) {
    interrupted = true;
    return TRUE;
  }
  return FALSE;
}
#else
void SignalHandler(int /*signum*/) {
  interrupted = true;
}
#endif

}  // namespace

void SetupSignalHandler() {
#ifdef _WIN32
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
#endif
}

bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

backend::ChatCallback StreamingRenderer::Create(bool show_reasoning) {
  ClearInterruptFlag();

  bool first_reasoning = true;

  return [show_reasoning, first_reasoning](backend::TokenType type,
                                           std::string_view token,
                                           bool is_final) mutable {
    OnToken(type, token, is_final, show_reasoning, first_reasoning);
  };
}

void StreamingRenderer::OnToken(backend::TokenType type,
                                std::string_view token,
                                bool is_final,
                                bool show_reasoning,
                                bool& first_reasoning) {
  if (type == backend::TokenType::kReasoning) {
    if (show_reasoning) {
      if (first_reasoning) {
        std::cerr << "[Thinking] ";
        first_reasoning = false;
      }
      std::cerr << token << std::flush;
      if (is_final) std::cerr << std::endl;
    }
    return;
  }

  if (type == backend::TokenType::kContent) {
    std::cout << token << std::flush;
    if (is_final) std::cout << std::endl;
  }
}

}  // namespace pu
