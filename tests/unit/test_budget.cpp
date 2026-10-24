// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/runtime/executor.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/toolbox.hpp"

#include <boost/json.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pu;

namespace {

// A model that calls one tool per round while its script says so, and then answers. It is the
// shape the budget exists for: nothing in the reply asks to stop, so only a count ends it.
class ScriptedLLM : public LLMProvider {
 public:
  // `calling_rounds` is how many rounds call the tool; after that it answers.
  ScriptedLLM(int calling_rounds, std::string final_content = "done")
      : calling_rounds_(calling_rounds), final_content_(std::move(final_content)) {}

  ChatResult Chat(const ChatRequest& request) override {
    ++calls;
    last_history.assign(request.history.begin(), request.history.end());

    ChatResult r;
    if (rounds_started++ < calling_rounds_) {
      ToolCall call;
      call.id = "call_" + std::to_string(calls);
      call.name = "counting_tool";
      call.arguments = boost::json::object{};
      r.tool_calls = {call};
      return r;
    }
    r.content = final_content_;
    return r;
  }

  int calls = 0;
  int rounds_started = 0;
  std::vector<ChatMessage> last_history;

 private:
  int calling_rounds_;
  std::string final_content_;
};

class CountingTool : public Tool {
 public:
  std::string Name() const override { return "counting_tool"; }
  std::string Description() const override { return "counts"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    ++executions;
    return R"({"success":true,"blocked":false,"output":"ran","error":"","exit_code":0})";
  }

  int executions = 0;
};

struct Fixture {
  Toolbox toolbox;
  Executor executor;

  Fixture() {
    toolbox.RegisterTool(std::make_unique<CountingTool>());
    config::SecurityPolicy policy;
    policy.sandbox_root = ".";
    executor.SetSecurityPolicy(policy);
  }
};

bool HistoryHasInstruction(const std::vector<ChatMessage>& history, const std::string& needle) {
  for (const ChatMessage& message : history) {
    if (message.role == "user" && message.content.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// The order the loop runs in, which these numbers record: the budget is checked at the top of a
// round, so the round whose reply *reaches* it has already run. With a budget of 2 the script is
// asked to call in rounds 1 and 2, and the instruction arrives before round 3. A script that
// still calls in round 3 spends the wrap-up and no further round is spent on it; a script that
// answers there ends the turn with `kCompleted`.
TEST_CASE("A turn that reaches its tool budget is asked to wrap up and does", "[budget]") {
  Fixture f;
  f.executor.SetToolCallBudget(2);

  // Rounds: call, call (the budget is reached), instruction, answer.
  ScriptedLLM model(/*calling_rounds=*/2, "wrapped up");
  Conversation conversation;

  const ExecutionResult result = f.executor.Execute("go", conversation, &model, f.toolbox);

  CHECK_FALSE(result.has_error);
  CHECK(result.content == "wrapped up");
  CHECK(result.outcome == TurnOutcome::kCompleted);
  CHECK(result.tool_call_count == 2);
  CHECK(model.calls == 3);
  CHECK(HistoryHasInstruction(model.last_history, "tool-call budget"));
}

// The wrap-up round calls a tool anyway. No further round is spent on it: the turn ends with
// `kToolBudgetReached` and an error naming the setting, rather than looping.
TEST_CASE("A wrap-up that still calls a tool ends the turn instead of looping", "[budget]") {
  Fixture f;
  f.executor.SetToolCallBudget(1);

  ScriptedLLM model(/*calling_rounds=*/5, "never reached");
  Conversation conversation;

  const ExecutionResult result = f.executor.Execute("go", conversation, &model, f.toolbox);

  CHECK(result.has_error);
  CHECK(result.outcome == TurnOutcome::kToolBudgetReached);
  CHECK(result.content.find("tool_call_budget") != std::string::npos);
  // Rounds: the call that reaches the budget, then the wrap-up round. The script calls in both,
  // so the second reply fails to answer and the turn ends there.
  CHECK(result.tool_call_count == 2);
  CHECK(model.calls == 2);
}

// A turn that finishes inside its budget never sees the instruction.
TEST_CASE("A turn inside its budget never sees the wrap-up", "[budget]") {
  Fixture f;
  f.executor.SetToolCallBudget(10);

  ScriptedLLM model(/*calling_rounds=*/1, "finished");
  Conversation conversation;

  const ExecutionResult result = f.executor.Execute("go", conversation, &model, f.toolbox);

  CHECK_FALSE(result.has_error);
  CHECK(result.content == "finished");
  CHECK(result.outcome == TurnOutcome::kCompleted);
  CHECK(result.tool_call_count == 1);
  CHECK_FALSE(HistoryHasInstruction(model.last_history, "tool-call budget"));
}

// One reply carrying two calls counts as two, and the round that reaches the budget still runs:
// a session that sets 2 gets two calls, not one.
TEST_CASE("A budget of two allows two calls", "[budget]") {
  Fixture f;
  f.executor.SetToolCallBudget(2);

  ScriptedLLM model(/*calling_rounds=*/1, "after two");
  Conversation conversation;

  const ExecutionResult result = f.executor.Execute("go", conversation, &model, f.toolbox);

  CHECK(result.tool_call_count == 1);  // one round, one call
  CHECK(model.calls == 2);
  CHECK_FALSE(result.has_error);
}
