// SPDX-License-Identifier: GPL-3.0-only
#include "pu/renderer.hpp"

#include "pu/infra/platform.hpp"

#include <iostream>

namespace pu {

void SetupSignalHandler() { platform::SetupSignalHandler(); }
bool IsInterrupted() { return platform::IsInterrupted(); }
void ClearInterruptFlag() { platform::ClearInterruptFlag(); }

StreamingRenderer::ContentCallback StreamingRenderer::Create() {
  return [](std::string_view token, bool is_final) {
    OnToken(token, is_final);
  };
}

void StreamingRenderer::OnToken(std::string_view token, bool is_final) {
  std::cout << token << std::flush;
  if (is_final) std::cout << '\n';
}

}  // namespace pu