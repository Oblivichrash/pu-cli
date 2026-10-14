// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::llm {

struct Capabilities {
  bool supports_tools = false;
  bool supports_images = false;
  bool supports_reasoning = false;
  // Measured: a request that carries no effort is answered 400 `11150`.
  bool only_reasoning = false;
  std::string default_effort;
  int max_input_tokens = 0;
};

struct ModelEntry {
  std::string id;
  Capabilities caps;
};

}  // namespace pu::llm
