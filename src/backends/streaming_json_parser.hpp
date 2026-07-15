// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pu::backends {

class StreamingJsonParser {
 public:
  using LineCallback = std::function<void(std::string_view)>;
  using ErrorCallback = std::function<void(const std::string&)>;

  StreamingJsonParser(LineCallback on_line, ErrorCallback on_error);
  void Feed(const char* data, size_t len);

 private:
  static bool IsPartialUtf8(std::string_view str);
  std::string buffer_;
  LineCallback on_line_;
  ErrorCallback on_error_;
};

}  // namespace pu::backends
