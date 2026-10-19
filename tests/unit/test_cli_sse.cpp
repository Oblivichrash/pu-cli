// SPDX-License-Identifier: GPL-3.0-only

//
// `pu sse`: what a caller's pipe reads.
//
// The formatter is the contract — a stream becomes text, and the three printings differ only in
// how much of the framing they carry. These cases drive it directly: what a feed looks like is
// settled by the parser's own cases, and what it prints is settled here.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>

#include "pu/app/cli.hpp"
#include "pu/core/json.hpp"
#include "pu/core/sse.hpp"

namespace {

pu::cli::SseOptions Options() {
  pu::cli::SseOptions options;
  options.url = "https://feed.invalid/events";
  return options;
}

pu::core::SseEvent EventWith(std::string event, std::string id, std::string data) {
  pu::core::SseEvent out;
  out.event = std::move(event);
  out.id = std::move(id);
  out.data = std::move(data);
  return out;
}

}  // namespace

TEST_CASE("An event prints its framing above its payload", "[sse][format]") {
  const std::string text =
      pu::cli::FormatSseEvent(EventWith("tick", "42", "{\"v\":1}"), Options());
  REQUIRE(text == "event: tick  id: 42\n{\"v\":1}");
}

TEST_CASE("An event that names nothing prints only its payload", "[sse][format]") {
  // The format leaves an event unnamed by default, and a line saying nothing above the payload
  // is a line a reader has to skip.
  const std::string text = pu::cli::FormatSseEvent(EventWith("", "", "hello"), Options());
  REQUIRE(text == "hello");
}

TEST_CASE("A quiet run leaves the payload alone", "[sse][format]") {
  pu::cli::SseOptions options = Options();
  options.quiet = true;
  const std::string text = pu::cli::FormatSseEvent(EventWith("tick", "42", "hello"), options);
  REQUIRE(text == "hello");
}

TEST_CASE("The JSON printing is one object per line, and omits what the server did not name",
          "[sse][format]") {
  pu::cli::SseOptions options = Options();
  options.as_json = true;

  const std::string named =
      pu::cli::FormatSseEvent(EventWith("tick", "42", "{\"v\":1}"), options);
  // One line: a caller pipes one event into one `jq`.
  REQUIRE(named.find('\n') == std::string::npos);

  const boost::json::value parsed = boost::json::parse(named);
  REQUIRE(pu::json::ValueOrDefault<std::string>(parsed, "event", "") == "tick");
  REQUIRE(pu::json::ValueOrDefault<std::string>(parsed, "id", "") == "42");
  REQUIRE(pu::json::ValueOrDefault<std::string>(parsed, "data", "") == "{\"v\":1}");

  // An unnamed event carries no `event` key rather than an empty one, so a filter on it is not
  // answering a question the server never asked.
  const boost::json::value unnamed =
      boost::json::parse(pu::cli::FormatSseEvent(EventWith("", "", "hello"), options));
  REQUIRE_FALSE(unnamed.as_object().contains("event"));
  REQUIRE_FALSE(unnamed.as_object().contains("id"));
  REQUIRE(pu::json::ValueOrDefault<std::string>(unnamed, "data", "") == "hello");
}
