// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/llm/probe.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

// An OpenAI-format stream whose usage frame arrives split over two `data:` lines, so the
// summary can only be right if the join the reader itself does is repeated here.
const char* kOpenAIStream =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"pong\","
    "\"reasoning_content\":\"thinking\"},\"finish_reason\":null}]}\n"
    "\n"
    // One usage payload split over two `data:` lines, the way a gateway that flushes
    // mid-object sends it: the fragments join, and the summary reads the joined frame.
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":3,\"prompt_tokens_details\":"
    "{\"cached_tokens\":2},\"comp"
    "data: letion_tokens\":1,\"total_tokens\":4,\"credit\":9}}\n"
    "\n"
    "data: [DONE]\n";

const char* kOllamaStream =
    "{\"model\":\"the-model\",\"message\":{\"role\":\"assistant\",\"content\":\"pong\"}}\n";

}  // namespace

TEST_CASE("[probe] an openai stream names its fields at each level", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw = kOpenAIStream;
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const llm::ProbeReport report =
      llm::ProbeStream(BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock,
                       "Say the single word: pong", std::nullopt);

  REQUIRE(mock->last_url == "https://x.example/v1/chat/completions");
  REQUIRE(report.stream_parsed);
  REQUIRE(report.frames == 2);
  REQUIRE(report.keys.at("delta").count("reasoning_content") == 1);
  REQUIRE(report.keys.at("usage").count("total_tokens") == 1);
  REQUIRE(report.keys.at("usage").count("prompt_tokens") == 1);
  REQUIRE(report.keys.at("usage.prompt_tokens_details").count("cached_tokens") == 1);
}

TEST_CASE("[probe] the shape diff names only what the stream never sent", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw = kOpenAIStream;
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const llm::ProbeReport report =
      llm::ProbeStream(BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock,
                       "Say the single word: pong", std::nullopt);

  // content, reasoning, the cached count (nested), total and credit were all named;
  // refusal and the completion count (which the split fragment's tail carried) were not.
  REQUIRE(report.shape_fields_absent ==
          std::vector<std::string>{"refusal", "completion_tokens"});
}

TEST_CASE("[probe] a top-level cached count satisfies the nested field", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw =
        "data: {\"choices\":[{\"delta\":{\"content\":\"pong\"}}],"
        "\"usage\":{\"prompt_tokens\":3,\"prompt_cache_hit_tokens\":2,"
        "\"completion_tokens\":1,\"total_tokens\":4}}\n"
        "\n";
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const llm::ProbeReport report =
      llm::ProbeStream(BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock,
                       "Say the single word: pong", std::nullopt);
  REQUIRE(report.stream_parsed);
  REQUIRE(std::find(report.shape_fields_absent.begin(), report.shape_fields_absent.end(),
                    std::string{"cached_tokens"}) == report.shape_fields_absent.end());
}

TEST_CASE("[probe] an ollama stream reads its own shape", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw = kOllamaStream;
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const llm::ProbeReport report =
      llm::ProbeStream(BackendOfType(config::GatewayId::kOllama, "http://localhost:11434"), *mock,
                       "Say the single word: pong", std::nullopt);

  REQUIRE(mock->last_url == "http://localhost:11434/api/chat");
  REQUIRE(report.frames == 1);
  REQUIRE(report.keys.at("message").count("content") == 1);
  // No ResponseShape to diff against a wire format that is not OpenAI's.
  REQUIRE(report.shape_fields_absent.empty());
}

TEST_CASE("[probe] a stream no frame of which parses says so", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw = "<html>gateway error</html>\n";
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const llm::ProbeReport report =
      llm::ProbeStream(BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock,
                       "Say the single word: pong", std::nullopt);

  REQUIRE_FALSE(report.stream_parsed);
  REQUIRE(report.frames == 0);
  REQUIRE(report.shape_fields_absent.empty());
}

TEST_CASE("[probe] the raw stream lands where --out names", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_response = [](const std::string&, const std::string&,
                               const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string raw = kOpenAIStream;
    std::string copy = raw; cb(copy.data(), copy.size());
  };

  const auto path = std::filesystem::temp_directory_path() / "pu_probe_test.raw";
  std::filesystem::remove(path);

  llm::ProbeStream(BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock,
                   "Say the single word: pong", path.string());

  REQUIRE(std::filesystem::file_size(path) == std::string(kOpenAIStream).size());
  std::filesystem::remove(path);
}

TEST_CASE("[probe] a listing is walked the way its shape reads it", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{
        200, R"({"object":"list","data":[
          {"id":"m-one","owned_by":"vendor","context_window":4096,"effort":{"default_level":"high"}},
          {"id":"m-two","owned_by":"vendor"}
        ]})"};
  };

  const llm::ListingReport report = llm::ProbeListing(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock);

  REQUIRE(report.parsed);
  REQUIRE(report.http_status == 200);
  REQUIRE(report.entries == 2);
  REQUIRE(report.url == "https://x.example/v1/models");
  REQUIRE(report.keys.at("top-level").count("data") == 1);
  // Both entries are walked: a field one of them carries is a field this gateway sends.
  REQUIRE(report.keys.at("entry").count("context_window") == 1);
  REQUIRE(report.keys.at("entry.effort").count("default_level") == 1);
}

TEST_CASE("[probe] a listing is asked for the names its shape reads", "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"m"}]})"};
  };

  const llm::ListingReport report = llm::ProbeListing(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock);

  // The OpenAI shape names no capability field, so nothing is reported absent: a shape that
  // does not ask is not a gateway that dropped anything.
  REQUIRE(report.shape_fields_absent.empty());
}

TEST_CASE("[probe] a listing that is not JSON says so rather than reporting an empty shape",
          "[probe]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, "<html>not json</html>"};
  };

  const llm::ListingReport report = llm::ProbeListing(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock);

  REQUIRE_FALSE(report.parsed);
  REQUIRE(report.error.find("does not parse as JSON") != std::string::npos);
  REQUIRE(report.shape_fields_absent.empty());
}
