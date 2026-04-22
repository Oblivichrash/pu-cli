// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "pu/renderer.hpp"

#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pu {

namespace {

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

bool IsInterrupted() {
  return interrupted;
}

void ClearInterruptFlag() {
  interrupted = false;
}

// Static members
std::string StreamingRenderer::accumulated_;
bool StreamingRenderer::first_content_token_ = true;

backend::ChatCallback StreamingRenderer::Create(bool show_reasoning) {
  accumulated_.clear();
  first_content_token_ = true;
  ClearInterruptFlag();

  // Capture per-request state for reasoning prefix
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
      if (is_final) {
        std::cerr << std::endl;
      }
    }
    return;
  }

  if (type == backend::TokenType::kContent) {
    if (first_content_token_) {
      first_content_token_ = false;
    }
    accumulated_ += token;
    std::cout << "\r" << accumulated_ << std::flush;
    if (is_final) {
      std::cout << std::endl;
      accumulated_.clear();
      first_content_token_ = true;
    }
  }
}

}  // namespace pu
