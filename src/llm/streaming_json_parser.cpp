// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/streaming_json_parser.hpp"

#include "pu/core/text.hpp"

namespace pu::llm {

StreamingJsonParser::StreamingJsonParser(LineCallback on_line) : on_line_(std::move(on_line)) {}

void StreamingJsonParser::Feed(const char* data, size_t len) {
  buffer_.append(data, len);
  while (true) {
    auto pos = buffer_.find('\n');
    if (pos == std::string::npos) break;
    std::string line = buffer_.substr(0, pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      buffer_.erase(0, pos + 1);
      continue;
    }
    // A newline can arrive in the middle of a character when the bytes were split
    // across two reads, so a line whose tail is only the start of a sequence waits for
    // the rest of it rather than being parsed as it stands.
    if (text::EndsWithPartialSequence(line)) break;
    buffer_.erase(0, pos + 1);
    on_line_(line);
  }
}

}  // namespace pu::llm
