// SPDX-License-Identifier: GPL-3.0-only

//
// `pu sse`: read a server-sent event stream and print what arrives.
//
// It is a general client: the URL, the method and the headers are the caller's, and nothing is
// read from agents.json. A feed is not a chat turn, and a client that quietly borrowed a
// gateway's key would be one whose requests a reader could not predict.
//
// The implementation lives here rather than beside a `main` because two entry points reach it:
// `pu sse` and the standalone `pu-sse`. A second copy would be a second one to fix.
//

#include "pu/tools_cli/sse.hpp"

#include <boost/json.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"

namespace pu::tools_cli {

std::string FormatSseEvent(const core::SseEvent& event, const SseOptions& options) {
  if (options.format == SseFormat::kJson) {
    // One object per line, so `| jq -r .data` is the whole pipeline a caller needs.
    boost::json::object out{{"data", event.data}};
    if (!event.event.empty()) out["event"] = event.event;
    if (!event.id.empty()) out["id"] = event.id;
    return boost::json::serialize(out);
  }

  std::ostringstream out;
  if (!options.quiet && (!event.event.empty() || !event.id.empty())) {
    if (!event.event.empty()) out << "event: " << event.event << "  ";
    if (!event.id.empty()) out << "id: " << event.id;
    out << "\n";
  }
  out << event.data;
  return out.str();
}

ExitCode RunSse(const SseOptions& options, http::HttpClient& http) {
  // The body is what the caller gave; a GET carries none, and the framing does not care.
  const std::string body = options.body.value_or("");

  bool first = true;
  core::SseParser parser([&](const core::SseEvent& event) {
    if (!first) std::cout << "\n";
    first = false;
    std::cout << FormatSseEvent(event, options) << std::flush;
  });

  try {
    http.StreamRequest(
        options.method, options.url, body, options.headers,
        [&](char* data, size_t size) -> size_t {
          // The raw shape is the bytes as sent, for a reader who wants to see the framing
          // itself.
          if (options.format == SseFormat::kRaw) {
            std::cout.write(data, static_cast<std::streamsize>(size));
            std::cout.flush();
          } else {
            parser.Feed(data, size);
          }
          return size;
        },
        platform::ShutdownToken());
  } catch (const std::exception& e) {
    // A stream a person interrupted is not the client failing: the events already delivered
    // stay on the page, and the status says a person stopped it.
    if (platform::ShutdownRequested()) {
      if (options.format != SseFormat::kRaw) {
        parser.Finish();
        std::cout << "\n";
      }
      return ExitCode::kInterrupted;
    }
    std::cerr << text::ReadableError(e.what()) << "\n";
    return ExitCode::kProvider;
  }
  if (options.format != SseFormat::kRaw) {
    parser.Finish();
    std::cout << "\n";
  }
  return ExitCode::kOk;
}

}  // namespace pu::tools_cli
