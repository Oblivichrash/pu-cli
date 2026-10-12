// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "pu/core/text.hpp"

namespace pu::llm {

// Splits a byte stream into complete lines; a line that does not parse is the callback's
// to handle, since only the caller knows what the stream was meant to say.
class StreamingJsonParser {
 public:
  using LineCallback = std::function<void(std::string_view)>;

  explicit StreamingJsonParser(LineCallback on_line) : on_line_(std::move(on_line)) {}

  void Feed(const char* data, size_t len) {
    buffer_.append(data, len);
    while (true) {
      const auto pos = buffer_.find('\n');
      if (pos == std::string::npos) break;
      std::string line = buffer_.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) {
        buffer_.erase(0, pos + 1);
        continue;
      }
      // A newline can fall in the middle of a character when the bytes were split across
      // two reads, so a line whose tail is a partial sequence waits for the rest of it.
      if (text::EndsWithPartialSequence(line)) break;
      buffer_.erase(0, pos + 1);
      on_line_(line);
    }
  }

 private:
  std::string buffer_;
  LineCallback on_line_;
};

}  // namespace pu::llm
