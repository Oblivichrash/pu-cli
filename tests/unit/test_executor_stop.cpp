// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/executor.hpp"
#include "pu/core/base.hpp"
#include "pu/session/message.hpp"
#include "pu/tools/tool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The two stops an executor did not previously have a way to hear: a deadline, which nobody
// presses a button for, and a shutdown, which has no token to cancel. Both arrive through one
// check, and where it is asked is the whole point — a stop that is only read at the top of a
// round still lets a tool call run, and one read only between tool calls still lets a round's
// reply through.

using namespace pu;

namespace {

// A model that always asks for another tool call, so a turn would run forever without a stop.
class EndlessLLM : public LLMProvider {
 public:
  ChatResult Chat(const ChatRequest& /*request*/) override {
    ++rounds;
    ChatResult r;
    ToolCall call;
    call.id = "call_" + std::to_string(rounds);
    call.name = "counting_tool";
    call.arguments = boost::json::object{};
    r.tool_calls = {call};
    return r;
  }

  int rounds = 0;
};

class CountingTool : public Tool {
 public:
  std::string Name() const override { return "counting_tool"; }
  std::string Description() const override { return "counts how often it ran"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    ++executions;
    return R"({"success":true,"blocked":false,"output":"ran","error":"","exit_code":0})";
  }

  int executions = 0;
};

// A check that answers false to the first `go_ahead` questions and true afterwards: a deadline
// that passes part-way through a turn. The questions are counted from one, so `go_ahead` is
// exactly how many stops-free moments the turn gets.
std::function<bool()> StopsAfter(int go_ahead, int* seen = nullptr) {
  auto allowed = std::make_shared<std::atomic<int>>(go_ahead);
  return [allowed, seen]() {
    if (seen != nullptr) ++*seen;
    return allowed->fetch_sub(1) <= 0;
  };
}

}  // namespace

TEST_CASE("A stop read between rounds ends the turn before the next reply", "[executor][stop]") {
  Toolbox toolbox;
  auto counting = std::make_unique<CountingTool>();
  auto* counted = counting.get();
  toolbox.RegisterTool(std::move(counting));

  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  EndlessLLM model;
  // The first two questions are the two rounds: the one that opens the turn and the round that
  // follows the first tool call. The third is asked before a reply, so the turn ends having run
  // one tool and having asked the model once.
  int asked = 0;
  executor.SetStopCheck(StopsAfter(2, &asked));

  Conversation ws;
  const ExecutionResult result = executor.Execute("go", ws, {.provider = &model, .toolbox = &toolbox});

  REQUIRE(counted->executions == 1);
  // A stopped turn is not a failed one: nothing has_error here, and the content is empty
  // because the model never got to say anything.
  REQUIRE_FALSE(result.has_error);
  REQUIRE(result.content.empty());
}

TEST_CASE("A stop read before a tool call keeps the rest of that round's calls from running",
          "[executor][stop]") {
  // One round, three calls. The stop is asked once at the top of the round and then once per
  // call, so a budget of two questions lets the round open and the first call run, and stops
  // the other two.
  class ThreeCallLLM : public LLMProvider {
   public:
    ChatResult Chat(const ChatRequest& /*request*/) override {
      ++rounds;
      if (rounds > 1) return {};
      ChatResult r;
      for (int i = 0; i < 3; ++i) {
        ToolCall call;
        call.id = "call_" + std::to_string(i);
        call.name = "counting_tool";
        call.arguments = boost::json::object{};
        r.tool_calls.push_back(call);
      }
      return r;
    }
    int rounds = 0;
  };

  Toolbox toolbox;
  auto counting = std::make_unique<CountingTool>();
  auto* counted = counting.get();
  toolbox.RegisterTool(std::move(counting));

  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);
  executor.SetStopCheck(StopsAfter(2));

  ThreeCallLLM model;
  Conversation ws;
  const ExecutionResult result = executor.Execute("go", ws, {.provider = &model, .toolbox = &toolbox});

  // The first call ran; the second was refused the moment the check answered.
  REQUIRE(counted->executions == 1);
  REQUIRE(result.tool_call_count == 1);
}

TEST_CASE("A turn with no stop check runs to its own end", "[executor][stop]") {
  // The default is the behaviour that existed before the check did, and a runtime that is not
  // delegated leaves it unset: a check that stopped turns nobody asked to stop would be the
  // same bug as a cancel that cancels the wrong one.
  Toolbox toolbox;
  auto tracking = std::make_unique<CountingTool>();
  auto* counted = tracking.get();
  toolbox.RegisterTool(std::move(tracking));

  class OneCallLLM : public LLMProvider {
   public:
    ChatResult Chat(const ChatRequest& request) override {
      ++rounds;
      ChatResult r;
      if (rounds == 1) {
        ToolCall call;
        call.id = "call_1";
        call.name = "counting_tool";
        call.arguments = boost::json::object{};
        r.tool_calls = {call};
      } else {
        r.content = "done";
      }
      (void)request;
      return r;
    }
    int rounds = 0;
  };

  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  OneCallLLM model;
  Conversation ws;
  const ExecutionResult result = executor.Execute("go", ws, {.provider = &model, .toolbox = &toolbox});

  REQUIRE(counted->executions == 1);
  REQUIRE(result.content == "done");
  REQUIRE_FALSE(result.has_error);
}
