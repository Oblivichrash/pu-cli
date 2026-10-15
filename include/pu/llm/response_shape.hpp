// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string_view>

namespace pu::llm {

// The names a provider gives the fields on a reply, as a counterpart to the names it
// gives them on a request. An empty name means the field does not exist on that gateway.
struct ResponseShape {
  std::string_view content_field = "content";
  std::string_view reasoning_field = "reasoning_content";
  std::string_view refusal_field = "refusal";

  std::string_view usage_field = "usage";
  std::string_view prompt_tokens_field = "prompt_tokens";
  std::string_view completion_tokens_field = "completion_tokens";
  std::string_view total_tokens_field = "total_tokens";
  std::string_view credit_field = "credit";

  // One gateway nests the cached prompt, another puts it at the top level; both carry the
  // same count. The nested name is tried first.
  std::string_view cached_tokens_parent = "prompt_tokens_details";
  std::string_view cached_tokens_nested_field = "cached_tokens";
  std::string_view cached_tokens_top_level_field = "prompt_cache_hit_tokens";

  std::string_view finish_reason_field = "finish_reason";
};

}  // namespace pu::llm
