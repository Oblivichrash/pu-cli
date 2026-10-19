// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>

#include <boost/json.hpp>

namespace pu::core {

// The fields are the ones the format names; see docs/providers.md#sse-framing.
struct SseEvent {
  std::string event;
  std::string id;
  std::string data;
};

// A pure function of the bytes fed to it. The framing questions a caller has to know —
// partial characters, an unterminated final frame, a bare line, `retry:` — are answered in
// docs/providers.md#sse-framing.
class SseParser {
 public:
  using EventCallback = std::function<void(const SseEvent&)>;
  using RetryCallback = std::function<void(int milliseconds)>;

  explicit SseParser(EventCallback on_event);

  void SetBareLinesAreEvents(bool enabled) { bare_lines_are_events_ = enabled; }

  void OnRetry(RetryCallback on_retry) { on_retry_ = std::move(on_retry); }

  void Feed(const char* data, size_t len);
  void Feed(const std::string& text) { Feed(text.data(), text.size()); }

  void Finish();

 private:
  void HandleLine(std::string_view line);
  void Dispatch();
  bool PendingIsComplete() const;

  EventCallback on_event_;
  RetryCallback on_retry_;

  std::string buffer_;
  bool bare_lines_are_events_ = false;
  SseEvent pending_;
  bool has_data_ = false;
};

}  // namespace pu::core
