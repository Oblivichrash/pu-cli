// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "pu/llm/projection.hpp"

using namespace pu::llm;
using pu::config::WireProtocol;

namespace {

// Every key one frame carries, filed the way a capture files it. The format is named by the
// caller, as it is in production: a probe passes its gateway's protocol, and its reader
// passes its own.
std::map<std::string, std::set<std::string>> Collect(const boost::json::value& frame,
                                                    WireProtocol protocol) {
  std::map<std::string, std::set<std::string>> keys;
  ForEachReplyKeyAtPath(
      frame, protocol, [&keys](std::string_view path, std::string_view key) {
        keys[std::string(path)].insert(std::string(key));
      });
  return keys;
}

}  // namespace

TEST_CASE("an OpenAI frame is filed at four paths", "[projection][shape]") {
  const boost::json::value frame = boost::json::parse(R"({
    "id": "x",
    "choices": [{"index": 0, "delta": {"content": "hi"}, "finish_reason": null}],
    "usage": {"prompt_tokens": 3, "prompt_tokens_details": {"cached_tokens": 2}}
  })");

  const auto keys = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(keys.at("top-level").count("id") == 1);
  REQUIRE(keys.at("choice").count("finish_reason") == 1);
  REQUIRE(keys.at("delta").count("content") == 1);
  REQUIRE(keys.at("usage").count("prompt_tokens") == 1);
  // A nested member is filed under the path that reaches it, not folded into `usage`: a
  // report that said only `usage` would hide which object `cached_tokens` came from.
  REQUIRE(keys.at("usage.prompt_tokens_details").count("cached_tokens") == 1);
  REQUIRE(keys.at("usage").count("cached_tokens") == 0);
}

TEST_CASE("a choice that carries a whole message is read at the delta level", "[projection][shape]") {
  const boost::json::value frame = boost::json::parse(
      R"({"choices": [{"message": {"role": "assistant", "content": "hi"}}]})");

  const auto keys = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(keys.at("delta").count("content") == 1);
  REQUIRE(keys.at("delta").count("role") == 1);
}

TEST_CASE("an Ollama frame files its own keys at the top level and its reply under message",
          "[projection][shape]") {
  const boost::json::value frame = boost::json::parse(
      R"({"model": "m", "message": {"content": "hi", "tool_calls": []}, "done": false})");

  const auto keys = Collect(frame, WireProtocol::kOllama);
  REQUIRE(keys.at("message").count("content") == 1);
  REQUIRE(keys.at("message").count("tool_calls") == 1);
  // `done`, `model` and the counts sit on the frame itself. Ollama has no `choices` and no
  // `usage`, so these are the top level of this wire format, not a fallback for a shape the
  // walk failed to recognize.
  REQUIRE(keys.at("top-level").count("done") == 1);
  REQUIRE(keys.at("top-level").count("model") == 1);
  REQUIRE(keys.count("choice") == 0);
  REQUIRE(keys.count("delta") == 0);
}

TEST_CASE("the two formats read the same frame differently", "[projection][shape]") {
  // An Ollama frame has no `choices`, so the OpenAI walk finds nothing below the top level
  // and says so instead of inventing a container. This is the point of naming the format:
  // the answer follows the gateway, not the bytes.
  const boost::json::value frame = boost::json::parse(R"({"message": {"content": "hi"}})");

  const auto as_ollama = Collect(frame, WireProtocol::kOllama);
  REQUIRE(as_ollama.at("message").count("content") == 1);

  const auto as_openai = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(as_openai.count("message") == 0);
  // `message` is still a top-level key of the frame, which is all this walk claims for a
  // format it was not told to read that way.
  REQUIRE(as_openai.at("top-level").count("message") == 1);
}

TEST_CASE("an OpenAI-only field is not read as an Ollama one", "[projection][shape]") {
  const boost::json::value frame =
      boost::json::parse(R"({"choices": [{"delta": {"content": "hi"}}]})");

  const auto as_openai = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(as_openai.at("delta").count("content") == 1);

  const auto as_ollama = Collect(frame, WireProtocol::kOllama);
  REQUIRE(as_ollama.count("delta") == 0);
  REQUIRE(as_ollama.at("top-level").count("choices") == 1);
}

TEST_CASE("a value that is not an object names no keys", "[projection][shape]") {
  REQUIRE(Collect(boost::json::value("just a string"), WireProtocol::kOpenAI).empty());
  REQUIRE(Collect(boost::json::value(nullptr), WireProtocol::kOpenAI).empty());
  REQUIRE(Collect(boost::json::parse(R"([{"a": 1}])"), WireProtocol::kOllama).empty());
}

TEST_CASE("an empty choices array names none of the levels under it", "[projection][shape]") {
  const boost::json::value frame = boost::json::parse(R"({"choices": [], "model": "m"})");

  const auto keys = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(keys.count("choice") == 0);
  REQUIRE(keys.count("delta") == 0);
  REQUIRE(keys.at("top-level").count("model") == 1);
}

TEST_CASE("a null usage and a null message name no nested keys", "[projection][shape]") {
  const boost::json::value frame = boost::json::parse(R"({"usage": null, "message": null})");

  const auto as_openai = Collect(frame, WireProtocol::kOpenAI);
  REQUIRE(as_openai.count("usage") == 0);
  REQUIRE(as_openai.at("top-level").count("usage") == 1);

  const auto as_ollama = Collect(frame, WireProtocol::kOllama);
  REQUIRE(as_ollama.count("message") == 0);
  REQUIRE(as_ollama.at("top-level").count("message") == 1);
}
