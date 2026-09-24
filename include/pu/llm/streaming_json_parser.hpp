// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pu::llm {

// Splits a byte stream into complete lines and hands each to `on_line`. It
// reports no errors of its own: only the caller knows what the stream was meant
// to say, so a line that does not parse is the callback's to handle.
class StreamingJsonParser {
 public:
  using LineCallback = std::function<void(std::string_view)>;

  explicit StreamingJsonParser(LineCallback on_line);
  void Feed(const char* data, size_t len);

 private:
  static bool IsPartialUtf8(std::string_view str);
  std::string buffer_;
  LineCallback on_line_;
};

}  // namespace pu::llm
