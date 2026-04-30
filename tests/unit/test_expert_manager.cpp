// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert.hpp"
#include "pu/conversation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

using namespace pu;
using namespace pu::expert;

namespace {

class MockExpert : public BaseExpert {
 public:
  explicit MockExpert(std::string name) : name_(std::move(name)) {}

  std::string Name() const override { return name_; }
  std::string Description() const override { return "Mock"; }
  std::string Handle(const std::string&, ExpertContext&) override {
    return "mock response";
  }
  void ResetSession() override { state_.clear(); }

  std::vector<ChatMessage> SaveState() const override {
    return state_;
  }
  void LoadState(const std::vector<ChatMessage>& messages) override {
    state_ = messages;
  }

  void AddFakeMessage(ChatMessage msg) {
    state_.push_back(std::move(msg));
  }

 private:
  std::string name_;
  std::vector<ChatMessage> state_;
};

}  // namespace

TEST_CASE("ExpertManager SnapshotExperts collects all states", "[expert]") {
  ExpertManager manager;

  auto expert1 = std::make_unique<MockExpert>("mock1");
  expert1->AddFakeMessage({1, "", "user", "hello"});
  expert1->AddFakeMessage({2, "", "mock1", "hi"});

  auto expert2 = std::make_unique<MockExpert>("mock2");
  expert2->AddFakeMessage({3, "", "user", "test"});

  manager.RegisterExpert(std::move(expert1));
  manager.RegisterExpert(std::move(expert2));

  auto snapshot = manager.SnapshotExperts();
  REQUIRE(snapshot.size() == 2);
  REQUIRE(snapshot["mock1"].size() == 2);
  REQUIRE(snapshot["mock2"].size() == 1);
  REQUIRE(snapshot["mock1"][0].content == "hello");
}

TEST_CASE("ExpertManager RestoreExperts loads states", "[expert]") {
  ExpertManager manager;

  auto expert = std::make_unique<MockExpert>("mock1");
  manager.RegisterExpert(std::move(expert));

  std::unordered_map<std::string, std::vector<ChatMessage>> states;
  states["mock1"] = {
    {1, "", "user", "restored message"}
  };

  manager.RestoreExperts(states);

  auto snapshot = manager.SnapshotExperts();
  REQUIRE(snapshot["mock1"].size() == 1);
  REQUIRE(snapshot["mock1"][0].content == "restored message");
}

TEST_CASE("ExpertManager RestoreExperts ignores unknown experts", "[expert]") {
  ExpertManager manager;

  std::unordered_map<std::string, std::vector<ChatMessage>> states;
  states["nonexistent"] = {{1, "", "user", "nobody"}};

  REQUIRE_NOTHROW(manager.RestoreExperts(states));
}

TEST_CASE("ExpertManager ClearSessions resets all states", "[expert]") {
  ExpertManager manager;

  auto expert = std::make_unique<MockExpert>("mock1");
  expert->AddFakeMessage({1, "", "user", "data"});
  manager.RegisterExpert(std::move(expert));

  manager.ClearSessions();

  auto snapshot = manager.SnapshotExperts();
  REQUIRE(snapshot["mock1"].empty());
}
