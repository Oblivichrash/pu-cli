// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/sse.hpp"

#include <cstdlib>
#include <string_view>
#include <utility>

#include "pu/core/text.hpp"

namespace pu::core {

namespace {

// "field: value", with one optional space after the colon removed. A line that is only a
// field name is that field with an empty value.
void SplitField(std::string_view line, std::string_view& field, std::string_view& value) {
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    field = line;
    value = {};
    return;
  }
  field = line.substr(0, colon);
  value = line.substr(colon + 1);
  if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
}

// Whether a line begins with a field name the format gives meaning to. A JSON object begins
// with `{` and a JSON array with `[`, so neither is mistaken for a field.
bool StartsAField(std::string_view line) {
  for (std::string_view field : {"data", "event", "id", "retry"}) {
    if (line.size() <= field.size()) continue;
    if (line.substr(0, field.size()) != field) continue;
    if (line[field.size()] == ':') return true;
  }
  return false;
}

// Whether the payload collected so far is a whole value. A JSON object, array or scalar parses
// as one; the `[DONE]` sentinel is whole by itself. What does not parse is a value still being
// assembled, and the next `data:` line is more of it.
bool ParsesAsOneValue(const std::string& data) {
  if (data.empty()) return false;
  if (data == "[DONE]") return true;
  boost::system::error_code ec;
  boost::json::parse(data, ec);
  return !ec;
}

}  // namespace

SseParser::SseParser(EventCallback on_event) : on_event_(std::move(on_event)) {}

void SseParser::Feed(const char* data, size_t len) {
  buffer_.append(data, len);
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline == std::string::npos) break;

    std::string line = buffer_.substr(0, newline);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // A newline can land in the middle of a character when the bytes were split across two
    // reads. Handing the half on would corrupt it, so the line waits for the rest.
    if (text::EndsWithPartialSequence(line)) break;

    buffer_.erase(0, newline + 1);
    HandleLine(line);
  }
}

void SseParser::HandleLine(std::string_view line) {
  if (line.empty()) {
    // The blank line ends the frame. It is also what a stream of frames separated by nothing
    // but their own blank lines sends between events, and an empty frame is not an event.
    Dispatch();
    return;
  }
  if (line.front() == ':') return;  // a comment, which a server uses to keep the socket alive

  if (bare_lines_are_events_ && !StartsAField(line)) {
    // Newline-delimited JSON: every line is a whole object and nothing is prefixed. The test
    // is whether the line begins with a name this format gives a field — `{"a": 1}` does not,
    // though it contains a colon, and `data: ...` does.
    SseEvent event;
    event.data = line;
    if (on_event_) on_event_(event);
    return;
  }

  std::string_view field;
  std::string_view value;
  SplitField(line, field, value);

  if (field == "data") {
    // A whole payload followed by another `data:` line is a new event. Senders that put one
    // object per line and skip the blank line between them are common enough that the two
    // readings have to be told apart; a payload that is not yet whole — a split JSON object,
    // or the sentinel — keeps collecting, which is the case the blank line exists for.
    if (has_data_ && PendingIsComplete()) Dispatch();
    // Several `data:` lines are one payload whose newlines were transport: joining them with
    // the newline they were split at is what restores it.
    if (has_data_) pending_.data += '\n';
    pending_.data.append(value);
    has_data_ = true;
    return;
  }
  if (field == "event") {
    pending_.event.assign(value);
    return;
  }
  if (field == "id") {
    // A frame that names an id makes it the stream's id; one that names it empty is the
    // format's way of saying the id is cleared, which is the same as having none.
    pending_.id.assign(value);
    return;
  }
  if (field == "retry") {
    try {
      if (on_retry_) {
        const int milliseconds = std::stoi(std::string(value));
        if (milliseconds >= 0) on_retry_(milliseconds);
      }
    } catch (const std::exception&) {
      // A retry hint that is not a number is a hint this build cannot use; the stream is not
      // about it.
    }
    return;
  }
  // `unknown field` is ignored by the format rather than refused, so a server may add one.
}

bool SseParser::PendingIsComplete() const { return ParsesAsOneValue(pending_.data); }

void SseParser::Dispatch() {
  // Whether or not this frame carried data, it is finished: the next one starts from nothing.
  // Leaving the flag set on the empty-frame path is what made `Finish` dispatch a frame
  // twice.
  const bool carried = has_data_;
  has_data_ = false;
  SseEvent event = std::move(pending_);
  pending_ = SseEvent{};
  // A frame with no data line is a name with nothing to deliver, which the format says is not
  // an event. `event` and `id` alone are exactly that.
  if (!carried) return;
  if (on_event_) on_event_(event);
}

void SseParser::Finish() {
  // The close terminates the last line: a server that stops after its final `data:` sent that
  // line, and the newline the protocol would have followed it with never arrives.
  if (!buffer_.empty()) {
    std::string line = buffer_;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    buffer_.clear();
    if (!text::EndsWithPartialSequence(line)) HandleLine(line);
  }
  // A frame that was never closed by a blank line is still an event if it carried data.
  Dispatch();
}

}  // namespace pu::core
