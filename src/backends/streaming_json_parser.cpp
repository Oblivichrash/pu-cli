// SPDX-License-Identifier: GPL-3.0-only

#include "streaming_json_parser.hpp"
#include "pu/error_codes.hpp"

namespace pu::backends {

StreamingJsonParser::StreamingJsonParser(LineCallback on_line,
                                         ErrorCallback on_error)
    : on_line_(std::move(on_line)), on_error_(std::move(on_error)) {}

void StreamingJsonParser::Feed(const char* data, size_t len) {
  buffer_.append(data, len);

  while (true) {
    auto pos = buffer_.find('\n');
    if (pos == std::string::npos) break;

    std::string line = buffer_.substr(0, pos);
    buffer_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (line.empty()) continue;

    if (IsPartialUtf8(line)) {
      buffer_ = line + "\n" + buffer_;
      break;
    }

    on_line_(line);
  }
}

bool StreamingJsonParser::IsPartialUtf8(std::string_view str) {
  if (str.empty()) return false;
  auto len = str.size();
  auto c = static_cast<unsigned char>(str.back());
  size_t expected = 0;
  if ((c & 0x80) == 0) {
    expected = 0;
  } else if ((c & 0xE0) == 0xC0) {
    expected = 2;
  } else if ((c & 0xF0) == 0xE0) {
    expected = 3;
  } else if ((c & 0xF8) == 0xF0) {
    expected = 4;
  } else {
    return false;
  }
  if (len < expected) return true;
  for (size_t i = len - expected + 1; i < len; ++i) {
    if ((static_cast<unsigned char>(str[i]) & 0xC0) != 0x80) return false;
  }
  return false;
}

}  // namespace pu::backends
