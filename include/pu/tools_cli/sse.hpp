// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// `pu sse`, and the entry point the standalone `pu-sse` binary shares with it.
//
// The two binaries call the one implementation: a copy kept for the second entry point would
// be a second one to fix, and the two would drift the first time one of them was read.
//

#include <optional>
#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/core/sse.hpp"
#include "pu/tools_cli/exit_code.hpp"

namespace pu::tools_cli {

// What `sse` prints. The three shapes are exclusive: raw bytes, one JSON object per event, or
// the event read as text -- so they are one choice rather than three flags that could be set
// together and contradict each other.
enum class SseFormat { kText, kJson, kRaw };

struct SseOptions {
  std::string url;
  pu::http::HttpMethod method = pu::http::HttpMethod::kGet;
  std::vector<std::string> headers;
  std::optional<std::string> body;
  SseFormat format = SseFormat::kText;
  // Whether the `event:` and `id:` lines are printed beside the data in the text shape.
  bool quiet = false;
};

// One event, in the shape the options asked for.
std::string FormatSseEvent(const core::SseEvent& event, const SseOptions& options);

// Reads the stream to its end, printing as it goes. `http` is the only thing this needs from
// the process that runs it: a session, a pager and a tool box are not part of reading a feed.
int RunSse(const SseOptions& options, http::HttpClient& http);

}  // namespace pu::tools_cli
