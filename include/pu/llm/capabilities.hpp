// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::llm {

// What a host says a model can do. A field the host does not report stays at the value
// below, which reads as "not known to support it" rather than as a measured no: a listing
// that names an id and nothing else gives an empty Capabilities, and a caller must treat
// that as unknown rather than as a refusal.
struct Capabilities {
  bool supports_tools = false;
  bool supports_images = false;
  bool supports_reasoning = false;
  // A model that may not be asked to stop reasoning. Measured on CodeBuddy: a request
  // carrying no effort is answered 400 `11150`, which is what the absence looks like.
  bool only_reasoning = false;
  // The effort the host itself names for this model, as a level name. Empty when the
  // host names none.
  std::string default_effort;
  int max_input_tokens = 0;
};

// A model the host reports, with what the host said about it. `caps` is empty when the
// listing named only the id.
struct ModelEntry {
  std::string id;
  Capabilities caps;
};

}  // namespace pu::llm
