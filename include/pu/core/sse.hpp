// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>

#include <boost/json.hpp>

namespace pu::core {

// One event out of a `text/event-stream`.
//
// The fields are the ones the format names. `event` is empty for a message, which is the
// default the format gives a frame that names no event; `id` is empty when the server sent
// none, and a reconnect that carries a `Last-Event-ID` is a reconnect to a server that did.
struct SseEvent {
  std::string event;
  std::string id;
  // Every `data:` line of the frame, joined with a newline: a payload split over several
  // lines is one payload, and the split is transport rather than content.
  std::string data;
};

// Splits a byte stream into events.
//
// It is a pure function of the bytes fed to it, so a caller can test it without a socket, and
// the same parser serves a chat gateway and a feed: the framing is a property of the format
// and not of who is sending.
//
// A `\n` can fall in the middle of a character when the bytes were split across two reads, so
// a line whose tail is a partial sequence waits for the rest of it rather than being handed on
// corrupted.
class SseParser {
 public:
  using EventCallback = std::function<void(const SseEvent&)>;
  // A `retry:` field is a hint for how long to wait before reconnecting. It is reported rather
  // than acted on: the caller owns the policy, and a parser that slept would be one a caller
  // could not test.
  using RetryCallback = std::function<void(int milliseconds)>;

  explicit SseParser(EventCallback on_event);

  // Some gateways answer with newline-delimited JSON instead of framed events: every line is a
  // whole object and nothing is prefixed. A stream that carries a `data:` line is an event
  // stream regardless, so the setting only decides what to do with a line that begins with no
  // field name — that line is delivered as an event whose payload is the line itself.
  void SetBareLinesAreEvents(bool enabled) { bare_lines_are_events_ = enabled; }

  void OnRetry(RetryCallback on_retry) { on_retry_ = std::move(on_retry); }

  // Any split of the stream is valid; the parser holds what is incomplete.
  void Feed(const char* data, size_t len);
  void Feed(const std::string& text) { Feed(text.data(), text.size()); }

  // The stream ended. A frame with no terminating blank line is still an event — a server that
  // closes after its last `data:` line sent that line — so it is delivered here rather than
  // dropped.
  void Finish();

 private:
  void HandleLine(std::string_view line);
  void Dispatch();
  // Whether the frame under construction already holds a whole value, which is what tells a
  // second `data:` line apart from more of the first one.
  bool PendingIsComplete() const;

  EventCallback on_event_;
  RetryCallback on_retry_;

  std::string buffer_;
  bool bare_lines_are_events_ = false;
  // The frame being built: a blank line is what says it is complete.
  SseEvent pending_;
  bool has_data_ = false;  // an event with no `data:` line is not one
};

}  // namespace pu::core
