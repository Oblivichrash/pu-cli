// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Terminal streaming renderer.

#pragma once

#include "pu/backend.hpp"

namespace pu {

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

class StreamingRenderer {
 public:
  // Create a ChatCallback that renders tokens to stdout. If show_reasoning is
  // true, reasoning tokens are printed to stderr with a "[Thinking]" prefix.
  static backend::ChatCallback Create(bool show_reasoning = false);

 private:
  static void OnToken(backend::TokenType type,
                      std::string_view token,
                      bool is_final,
                      bool show_reasoning,
                      bool& first_reasoning);
};

}  // namespace pu
