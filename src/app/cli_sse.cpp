// SPDX-License-Identifier: GPL-3.0-only

//
// `pu sse` as the `pu` command frame sees it.
//
// The work is in `tools_cli/sse.cpp`, which the standalone `pu-sse` reaches too. What is left
// here is the part that belongs to `pu`: a caller inside `pu` sees no difference. Two copies
// of the reader would be two to fix, and they would drift the first time only one was read.
//

#include "pu/app/cli.hpp"

#include "pu/tools_cli/sse.hpp"

namespace pu::cli {

std::string FormatSseEvent(const core::SseEvent& event, const SseOptions& options) {
  return tools_cli::FormatSseEvent(event, options);
}

int RunSse(const SseOptions& options, http::HttpClient& http) {
  return static_cast<int>(tools_cli::RunSse(options, http));
}

}  // namespace pu::cli
