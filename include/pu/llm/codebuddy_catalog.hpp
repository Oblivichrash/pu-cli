// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace pu::llm {

// The catalogue is on the chat base's origin, not under it: appending /v3/config to
// the chat base is answered 404.
inline std::string CodeBuddyCatalogUrl(const std::string& host) {
  constexpr std::string_view kVersionSegment = "/v2";
  std::string origin = host;
  if (origin.ends_with(kVersionSegment)) {
    origin.resize(origin.size() - kVersionSegment.size());
  }
  return origin + "/v3/config";
}

}  // namespace pu::llm
