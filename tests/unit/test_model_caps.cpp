// SPDX-License-Identifier: GPL-3.0-only

//
// The listing as a table, with the columns that decide between two models that both work.
//
// A listing answers "what can I pick" first and "which of these two" second, and the second
// question is the one that needs `only-reasoning`, the mandatory effort and the price. They
// are printed from the same renderer as the id list, so the two answers cannot disagree.
//

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/llm/model_listing.hpp"

namespace {

namespace llm = pu::llm;

llm::ModelEntry ModelWith(const std::string& id, const llm::Capabilities& caps) {
  llm::ModelEntry entry;
  entry.id = id;
  entry.caps = caps;
  return entry;
}

}  // namespace

TEST_CASE("A capabilities row says what the model accepts and where it must be told", "[caps]") {
  llm::Capabilities caps;
  caps.supports_tools = true;
  caps.supports_images = true;
  caps.supports_reasoning = true;
  // The flag that predicts a refusal: an effort is mandatory, and a request without one is
  // answered 400.
  caps.only_reasoning = true;
  caps.default_effort = "high";
  caps.max_input_tokens = 200000;
  caps.credits = "x0.17 credits";

  const std::vector<llm::ModelEntry> models = {ModelWith("deepseek-v4-pro", caps)};
  const std::string rendered =
      llm::FormatModelList(models, "codebuddy", "https://x.invalid/v2", "deepseek-v4-pro",
                           llm::ListingDetail::kCaps);

  REQUIRE(rendered.find("tools") != std::string::npos);
  REQUIRE(rendered.find("images") != std::string::npos);
  REQUIRE(rendered.find("reasoning") != std::string::npos);
  REQUIRE(rendered.find("only-reasoning") != std::string::npos);
  REQUIRE(rendered.find("effort=high") != std::string::npos);
  REQUIRE(rendered.find("max-in=200000") != std::string::npos);
  REQUIRE(rendered.find("x0.17 credits") != std::string::npos);
}

TEST_CASE("A column the gateway said nothing about is absent, not empty", "[caps]") {
  // Every field defaulted: this is a gateway that reports ids and nothing else.
  const std::vector<llm::ModelEntry> models = {ModelWith("plain", {})};
  const std::string rendered = llm::FormatModelList(models, "ollama", "http://127.0.0.1:1",
                                                    "plain", llm::ListingDetail::kCaps);

  REQUIRE(rendered.find("plain") != std::string::npos);
  REQUIRE(rendered.find("tools") == std::string::npos);
  REQUIRE(rendered.find("only-reasoning") == std::string::npos);
  REQUIRE(rendered.find("effort=") == std::string::npos);
  REQUIRE(rendered.find("max-in=") == std::string::npos);
}

TEST_CASE("The id list is unchanged by the capability renderer", "[caps]") {
  llm::Capabilities caps;
  caps.supports_tools = true;
  caps.credits = "x0.06 credits";
  const std::vector<llm::ModelEntry> models = {ModelWith("cheap", caps)};

  const std::string ids =
      llm::FormatModelList(models, "codebuddy", "https://x.invalid/v2", "cheap");
  const std::string with_caps = llm::FormatModelList(models, "codebuddy", "https://x.invalid/v2",
                                                     "cheap", llm::ListingDetail::kCaps);

  // The default is still the id and the price, which is what `models` printed before.
  REQUIRE(ids.find("cheap") != std::string::npos);
  REQUIRE(ids.find("x0.06 credits") != std::string::npos);
  REQUIRE(ids.find("tools") == std::string::npos);
  // The same renderer adds the columns when asked, and keeps the price.
  REQUIRE(with_caps.find("tools") != std::string::npos);
  REQUIRE(with_caps.find("x0.06 credits") != std::string::npos);
}

TEST_CASE("The active marker is the same column in both renderings", "[caps]") {
  llm::Capabilities caps;
  caps.supports_tools = true;
  const std::vector<llm::ModelEntry> models = {ModelWith("a", {}), ModelWith("the-active", caps)};

  const std::string with_caps = llm::FormatModelList(models, "openai", "https://x.invalid/v1",
                                                     "the-active", llm::ListingDetail::kCaps);
  // One marker, on the row that is running, and the columns still line up under it.
  REQUIRE(with_caps.find("the-active (active)") != std::string::npos);
  REQUIRE(with_caps.find("a (active)") == std::string::npos);
}
