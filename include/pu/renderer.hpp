// SPDX-License-Identifier: GPL-3.0-only
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
  static backend::ChatCallback Create(bool show_reasoning = false);

 private:
  static void OnToken(backend::TokenType type,
                      std::string_view token,
                      bool is_final,
                      bool show_reasoning,
                      bool& first_reasoning);
};

}  // namespace pu
