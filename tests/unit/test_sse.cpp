// SPDX-License-Identifier: GPL-3.0-only

//
// The event-stream framing, held still.
//
// A parser fed arbitrary byte splits has to produce the same events as one fed the whole
// stream at once. That is the property every case here leans on, and it is the one a socket
// exercises without warning: a read boundary is allowed to fall anywhere.
//

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/core/sse.hpp"

namespace {

std::vector<pu::core::SseEvent> Parse(const std::string& stream) {
  std::vector<pu::core::SseEvent> events;
  pu::core::SseParser parser([&](const pu::core::SseEvent& event) { events.push_back(event); });
  parser.Feed(stream);
  parser.Finish();
  return events;
}

// The same stream, fed one byte at a time: what a socket does when a packet lands mid-frame.
std::vector<pu::core::SseEvent> ParseBytewise(const std::string& stream) {
  std::vector<pu::core::SseEvent> events;
  pu::core::SseParser parser([&](const pu::core::SseEvent& event) { events.push_back(event); });
  for (char byte : stream) parser.Feed(&byte, 1);
  parser.Finish();
  return events;
}

}  // namespace

TEST_CASE("A frame ends at its blank line", "[sse]") {
  const std::vector<pu::core::SseEvent> events = Parse("data: one\n\ndata: two\n\n");

  REQUIRE(events.size() == 2);
  REQUIRE(events[0].data == "one");
  REQUIRE(events[1].data == "two");
  // A frame that names no event is a message, which the format leaves unnamed.
  REQUIRE(events[0].event.empty());
}

TEST_CASE("A payload split over several data lines is one payload", "[sse]") {
  // The split is transport: the newlines between the lines are what the sender meant to be
  // inside the payload, so joining them restores it.
  const std::vector<pu::core::SseEvent> events = Parse("data: {\"a\":\ndata: 1}\n\n");

  REQUIRE(events.size() == 1);
  REQUIRE(events[0].data == "{\"a\":\n1}");
}

TEST_CASE("An event names itself and its id", "[sse]") {
  const std::vector<pu::core::SseEvent> events =
      Parse("event: tick\nid: 42\ndata: {}\n\nevent: done\ndata: bye\n\n");

  REQUIRE(events.size() == 2);
  REQUIRE(events[0].event == "tick");
  REQUIRE(events[0].id == "42");
  REQUIRE(events[1].event == "done");
  // An id belongs to the frame that named it; the next frame does not inherit it.
  REQUIRE(events[1].id.empty());
}

TEST_CASE("A comment is not an event and carries nothing", "[sse]") {
  // A server sends these to keep a socket alive; a client that delivered them would report
  // traffic the sender never meant as content.
  const std::vector<pu::core::SseEvent> events = Parse(": keep-alive\n\ndata: real\n\n");

  REQUIRE(events.size() == 1);
  REQUIRE(events[0].data == "real");
}

TEST_CASE("A frame with no data line is not an event", "[sse]") {
  // The format says a frame is dispatched only if it carried data. An `event:` alone is a
  // name with nothing to deliver.
  const std::vector<pu::core::SseEvent> events = Parse("event: empty\n\n");
  REQUIRE(events.empty());
}

TEST_CASE("A retry hint is reported and not acted on", "[sse]") {
  std::vector<int> hints;
  pu::core::SseParser parser([](const pu::core::SseEvent&) {});
  parser.OnRetry([&](int milliseconds) { hints.push_back(milliseconds); });

  // The caller owns the policy: a parser that slept could not be tested, and one that decided
  // would ignore the flag a caller was given.
  parser.Feed("retry: 3000\nretry: not-a-number\ndata: x\n\n");
  parser.Finish();

  REQUIRE(hints.size() == 1);
  REQUIRE(hints[0] == 3000);
}

TEST_CASE("A stream that ends without its blank line still delivers its last frame", "[sse]") {
  // A server that closes after its final `data:` line sent that line.
  std::vector<pu::core::SseEvent> events;
  pu::core::SseParser parser([&](const pu::core::SseEvent& event) { events.push_back(event); });
  parser.Feed("data: last");
  REQUIRE(events.empty());  // nothing until the frame is known to be complete
  parser.Finish();

  REQUIRE(events.size() == 1);
  REQUIRE(events[0].data == "last");
}

TEST_CASE("The same stream split at every boundary reads the same", "[sse]") {
  // The property a socket exercises: a read boundary may fall anywhere, including inside a
  // multi-byte character, and the events must not change.
  const std::string stream =
      "event: tick\nid: 1\ndata: {\"\xE4\xB8\xAD\xE6\x96\x87\": 1}\n\n"
      ": keep-alive\n\n"
      "data: one\ndata: two\n\n"
      "data: last";

  const std::vector<pu::core::SseEvent> whole = Parse(stream);
  const std::vector<pu::core::SseEvent> bytewise = ParseBytewise(stream);

  REQUIRE(whole.size() == 3);
  REQUIRE(bytewise.size() == whole.size());
  for (std::size_t i = 0; i < whole.size(); ++i) {
    REQUIRE(whole[i].event == bytewise[i].event);
    REQUIRE(whole[i].id == bytewise[i].id);
    REQUIRE(whole[i].data == bytewise[i].data);
  }
  // The multi-byte payload survived the split in both readings.
  REQUIRE(whole[0].data == "{\"\xE4\xB8\xAD\xE6\x96\x87\": 1}");
}

TEST_CASE("A carriage return is framing, not content", "[sse]") {
  const std::vector<pu::core::SseEvent> events = Parse("data: one\r\n\r\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].data == "one");
}
