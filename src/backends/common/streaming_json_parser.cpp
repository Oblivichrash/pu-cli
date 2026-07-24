// SPDX-License-Identifier: GPL-3.0-only
#include "backends/common/streaming_json_parser.hpp"

namespace pu::backends {

StreamingJsonParser::StreamingJsonParser(LineCallback on_line, ErrorCallback on_error)
    : on_line_(std::move(on_line)), on_error_(std::move(on_error)) {}

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
    if (IsPartialUtf8(line)) break;
    buffer_.erase(0, pos + 1);
    on_line_(line);
  }
}

bool StreamingJsonParser::IsPartialUtf8(std::string_view str) {
  if (str.empty()) return false;
  size_t i = str.size(), remaining = 0;
  while (i > 0) {
    unsigned char c = static_cast<unsigned char>(str[--i]);
    if ((c & 0xC0) == 0x80) {
      ++remaining;
    } else if ((c & 0x80) == 0x00) {
      return false;
    } else {
      size_t expected = 1;
      if ((c & 0xE0) == 0xC0) expected = 2;
      else if ((c & 0xF0) == 0xE0) expected = 3;
      else if ((c & 0xF8) == 0xF0) expected = 4;
      return remaining < (expected - 1);
    }
  }
  return remaining > 0;
}

}  // namespace pu::backends
