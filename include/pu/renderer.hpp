// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string_view>

namespace pu {

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

class StreamingRenderer {
 public:
  using ContentCallback = std::function<void(std::string_view, bool)>;

  static ContentCallback Create();

 private:
  static void OnToken(std::string_view token, bool is_final);
};

}  // namespace pu