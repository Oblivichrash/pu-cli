// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Terminal streaming renderer with typewriter effect and interrupt support.

#pragma once

#include "pu/backend.hpp"
#include <atomic>
#include <string>

namespace pu {

// Setup signal handler for SIGINT (Ctrl+C). Should be called once at program start.
void SetupSignalHandler();

// Returns true if an interrupt (Ctrl+C) was requested.
bool IsInterrupted();

// Clear the interrupt flag (called automatically before each new request).
void ClearInterruptFlag();

// StreamingRenderer: wraps a ChatCallback to provide typewriter-style output.
class StreamingRenderer {
 public:
  // Create a ChatCallback that renders tokens to stdout. If show_reasoning is
  // true, reasoning tokens are printed to stderr with a "[Thinking]" prefix.
  // Default is false (ignore reasoning).
  // Each call to Create resets the internal rendering state.
  static backend::ChatCallback Create(bool show_reasoning = false);

  // This class uses static state and is not safe for concurrent use.

 private:
  static void OnToken(backend::TokenType type,
                      std::string_view token,
                      bool is_final,
                      bool show_reasoning,
                      bool& first_reasoning);

  static std::string accumulated_;
  static bool first_content_token_;
};

}  // namespace pu
