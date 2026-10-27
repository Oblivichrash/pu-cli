// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/executor.hpp"
#include "pu/core/base.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/message.hpp"
#include "pu/tools/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using Catch::Approx;
#include <boost/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pu;

TEST_CASE("BuildStaticSystemContext includes environment info", "[executor]") {
  Executor executor;
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Environment ===") != std::string::npos);
  REQUIRE(msg.find("OS: ") != std::string::npos);
  REQUIRE(msg.find("Kernel: ") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext carries a digest the caller supplies", "[executor]") {
  Executor executor;
  std::string msg = executor.BuildStaticSystemContext();
  CHECK(msg.find("=== Peer Board ===") == std::string::npos);

  executor.context_digest = [] { return std::string("=== Peer Board ===\n- j1 from B\n"); };
  msg = executor.BuildStaticSystemContext();

  CHECK(msg.find("=== Peer Board ===") != std::string::npos);
  CHECK(msg.find("- j1 from B") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext says nothing extra when the digest is empty", "[executor]") {
  Executor executor;
  const std::string without = executor.BuildStaticSystemContext();

  executor.context_digest = [] { return std::string(); };

  CHECK(executor.BuildStaticSystemContext() == without);
}

TEST_CASE("the digest is asked for on every round, not cached", "[executor]") {
  Executor executor;
  int asked = 0;
  executor.context_digest = [&asked] { return std::to_string(++asked); };

  executor.BuildStaticSystemContext();
  executor.BuildStaticSystemContext();

  CHECK(asked == 2);
}

TEST_CASE("BuildStaticSystemContext includes security policy when set", "[executor]") {  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp/sandbox";
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Security Policy ===") != std::string::npos);
  REQUIRE(msg.find("Sandbox root: /tmp/sandbox") != std::string::npos);
  REQUIRE(msg.find("Forbidden patterns: ") != std::string::npos);
  REQUIRE(msg.find("'rm -rf'") != std::string::npos);
  REQUIRE(msg.find("'sudo'") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext shows empty forbidden patterns correctly", "[executor]") {
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("Forbidden patterns: (none)") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext shows no-security-policy message when unset", "[executor]") {
  Executor executor;
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("(no security policy set)") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext includes working directory section", "[executor]") {
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/home/user/project";
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  REQUIRE(msg.find("/home/user/project") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext working directory defaults to dot", "[executor]") {
  Executor executor;
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  REQUIRE(msg.find(".\n") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext includes tool use guidelines", "[executor]") {
  Executor executor;
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Tool Use Guidelines ===") != std::string::npos);
  REQUIRE(msg.find("step-by-step plan") != std::string::npos);
  // Which end a read starts at is the model's call, so the prompt names both ends.
  REQUIRE(msg.find("view=tail") != std::string::npos);
  REQUIRE(msg.find("cursor") != std::string::npos);
  REQUIRE(msg.find("parallel tool calls") != std::string::npos);
}

TEST_CASE("What the environment probe found reaches the request", "[executor]") {
  Executor executor;
  const std::string context = executor.BuildStaticSystemContext();

  REQUIRE(context.find("=== Environment ===") != std::string::npos);
  REQUIRE(context.find("OS: \n") == std::string::npos);
  REQUIRE(context.find("Kernel: \n") == std::string::npos);
}

namespace {

class MockLLM : public LLMProvider {
 public:
  explicit MockLLM(std::vector<ToolCall> calls, std::string content = "",
                   bool fire_calls_once = false)
      : calls_(std::move(calls)), content_(std::move(content)), fire_calls_once_(fire_calls_once) {}

  ChatResult Chat(const ChatRequest& /*request*/) override {
    ChatResult r;
    r.content = content_;
    r.tool_calls = calls_;
    if (fire_calls_once_) calls_.clear();
    if (per_round_credit_ > 0.0) {
      TokenUsage usage;
      usage.prompt_tokens = per_round_prompt_;
      usage.total_tokens = per_round_prompt_;
      // A round the fake prices reports a cost in the product's own unit; one it does not
      // leaves the cost absent, which is what a gateway that says nothing sends.
      if (per_round_credit_ > 0.0) usage.cost = Cost{per_round_credit_, "credits"};
      r.usage = usage;
    }
    return r;
  }

  // Makes every round report the same usage, so a test can check the turn's result
  // sums the rounds rather than keeping the last one.
  void ReportUsagePerRound(int prompt_tokens, double credit) {
    per_round_prompt_ = prompt_tokens;
    per_round_credit_ = credit;
  }

 private:
  std::vector<ToolCall> calls_;
  std::string content_;
  bool fire_calls_once_ = false;
  int per_round_prompt_ = 0;
  double per_round_credit_ = 0.0;
};

class OfferingLLM : public LLMProvider {
 public:
  OfferingLLM(std::string wanted, std::string content = "")
      : wanted_(std::move(wanted)), content_(std::move(content)) {}

  ChatResult Chat(const ChatRequest& request) override {
    ChatResult r;
    r.content = content_;

    offered.clear();
    bool wanted_is_offered = false;
    for (const ToolDefinition& def : request.tools) {
      offered.push_back(def.name);
      if (def.name == wanted_) wanted_is_offered = true;
    }

    if (wanted_is_offered && !asked_) {
      asked_ = true;
      ToolCall call;
      call.name = wanted_;
      call.arguments = boost::json::object{};
      r.tool_calls = {call};
    }
    return r;
  }

  std::vector<std::string> offered;

 private:
  std::string wanted_;
  std::string content_;
  bool asked_ = false;
};

class FailingLLM : public LLMProvider {
 public:
  ChatResult Chat(const ChatRequest& /*request*/) override {
    throw pu::HttpError("HTTP error 400: maximum context length is 4096 tokens");
  }
};

class CapturingLLM : public LLMProvider {
 public:
  ChatResult Chat(const ChatRequest& request) override {
    history_.assign(request.history.begin(), request.history.end());
    ChatResult r;
    r.content = "done";
    return r;
  }

  const std::vector<ChatMessage>& captured() const { return history_; }

 private:
  std::vector<ChatMessage> history_;
};

class StoppingLLM : public LLMProvider {
 public:
  StoppingLLM(std::string content, std::string finish_reason, std::string model = "",
              std::string reasoning = "")
      : content_(std::move(content)),
        finish_reason_(std::move(finish_reason)),
        model_(std::move(model)),
        reasoning_(std::move(reasoning)) {}

  ChatResult Chat(const ChatRequest& request) override {
    if (request.on_reasoning && !reasoning_.empty()) request.on_reasoning(reasoning_);
    ChatResult r;
    r.content = content_;
    r.finish_reason = finish_reason_;
    r.model = model_;
    if (!reasoning_.empty()) r.reasoning.push_back(ReasoningBlock{reasoning_, {}, nullptr});
    return r;
  }

 private:
  std::string content_;
  std::string finish_reason_;
  std::string model_;
  std::string reasoning_;
};

class TrackingTool : public Tool {
 public:
  explicit TrackingTool(std::string name = "tracking_tool") : name_(std::move(name)) {}
  std::string Name() const override { return name_; }
  std::string Description() const override { return "records execution"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    ++executions;
    return R"({"success":true,"blocked":false,"output":"ran","error":"","exit_code":0})";
  }

  int executions = 0;

 private:
  std::string name_;
};

class NamedTool : public Tool {
 public:
  explicit NamedTool(std::string name) : name_(std::move(name)) {}
  std::string Name() const override { return name_; }
  std::string Description() const override { return "does nothing"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    ++executions;
    return R"({"success":true,"blocked":false,"output":"","error":"","exit_code":0})";
  }

  int executions = 0;

 private:
  std::string name_;
};

class TwoCallLLM : public LLMProvider {
 public:
  ChatResult Chat(const ChatRequest& request) override {
    if (asked_) {
      second_round_.assign(request.history.begin(), request.history.end());
      ChatResult r;
      r.content = "done";
      return r;
    }
    asked_ = true;
    ChatResult r;
    ToolCall first;
    first.id = "call_1";
    first.name = "noting_tool";
    first.arguments = boost::json::object{};
    ToolCall second;
    second.id = "call_2";
    second.name = "noting_tool";
    second.arguments = boost::json::object{};
    r.tool_calls = {first, second};
    return r;
  }

  const std::vector<ChatMessage>& second_round() const { return second_round_; }

 private:
  bool asked_ = false;
  std::vector<ChatMessage> second_round_;
};

// Stands in for a note raised while a tool runs. The page no longer raises one from a
// per-command button -- a cancel covers the whole turn -- but the store still carries a note
// to the model, and this is what pins the order it lands in.
class NotingTool : public Tool {
 public:
  NotingTool(Executor* executor, std::string note)
      : executor_(executor), note_(std::move(note)) {}
  std::string Name() const override { return "noting_tool"; }
  std::string Description() const override { return "raises the note"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    if (executor_ != nullptr) executor_->SetUserNote(note_);
    return R"({"success":true,"blocked":false,"output":"ran","error":"","exit_code":0})";
  }

 private:
  Executor* executor_;
  std::string note_;
};

}  // namespace

TEST_CASE("Executor fires tool_start/tool_end callbacks around tool execution",
          "[executor][tool_loop][tool_callbacks]") {
  Toolbox toolbox;
  auto tracking = std::make_unique<TrackingTool>();
  auto* tracking_ptr = tracking.get();
  toolbox.RegisterTool(std::move(tracking));

  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  ToolCall call;
  call.id = "";  // let Executor assign a generated id
  call.name = "tracking_tool";
  call.arguments = boost::json::value{{"flag", true}};

  MockLLM mock(std::vector<ToolCall>{call}, "done", /*fire_calls_once=*/true);

  std::vector<std::string> started_ids;
  std::vector<std::string> started_names;
  std::vector<boost::json::value> started_args;
  std::vector<std::string> ended_ids;
  std::vector<std::string> ended_names;
  std::vector<boost::json::value> ended_args;
  std::vector<std::string> ended_outputs;
  std::vector<std::string> ended_errors;

  ToolCallbacks cb;
  cb.on_start = [&](const std::string& id, const std::string& name,
                    const boost::json::value& args) {
    started_ids.push_back(id);
    started_names.push_back(name);
    started_args.push_back(args);
  };
  cb.on_end = [&](const std::string& id, const std::string& name,
                  const boost::json::value& args, const std::string& output,
                  const std::string& error) {
    ended_ids.push_back(id);
    ended_names.push_back(name);
    ended_args.push_back(args);
    ended_outputs.push_back(output);
    ended_errors.push_back(error);
  };

  Conversation ws;
  ExecutionResult result = executor.Execute("run it", ws, &mock, toolbox, nullptr, nullptr, cb);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "done");
  REQUIRE(tracking_ptr->executions == 1);

  REQUIRE(started_ids.size() == 1);
  REQUIRE(ended_ids.size() == 1);

  REQUIRE_FALSE(started_ids[0].empty());
  REQUIRE(started_ids[0] == ended_ids[0]);

  REQUIRE(started_names[0] == "tracking_tool");
  REQUIRE(started_args[0].is_object());
  REQUIRE(started_args[0].as_object().at("flag") == true);

  // The result carries the call's own name and arguments: a page that reconnected mid-command
  // is replayed the end without the start, and what it draws has to come from the end.
  REQUIRE(ended_names[0] == "tracking_tool");
  REQUIRE(ended_args[0].is_object());
  REQUIRE(ended_args[0].as_object().at("flag") == true);

  REQUIRE(ended_outputs[0] == "ran");
  REQUIRE(ended_errors[0].empty());

  bool found_paired_tool_msg = false;
  for (const auto& msg : ws.History()) {
    if (msg.role == "tool" && msg.tool_call_id == started_ids[0]) {
      found_paired_tool_msg = true;
      break;
    }
  }
  REQUIRE(found_paired_tool_msg);
}

TEST_CASE("A turn's usage sums what every round of the tool loop reported", "[executor][tool_loop][usage]") {
  Toolbox toolbox;
  auto tracking = std::make_unique<TrackingTool>();
  toolbox.RegisterTool(std::move(tracking));

  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  ToolCall call;
  call.name = "tracking_tool";
  call.arguments = boost::json::value{{"flag", true}};

  // Two rounds, each reporting its own usage: the tool-call round and the final one.
  MockLLM mock(std::vector<ToolCall>{call}, "done", /*fire_calls_once=*/true);
  mock.ReportUsagePerRound(/*prompt_tokens=*/100, /*credit=*/0.05);

  Conversation ws;
  ExecutionResult result = executor.Execute("run it", ws, &mock, toolbox);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "done");
  REQUIRE(result.usage.has_value());
  // Two rounds at 100 prompt tokens each: the turn's figure is the sum, not the
  // last round's 100.
  REQUIRE(result.usage->prompt_tokens == 200);
  // Credit is a per-call cost, so it adds across rounds the same way.
  // Two rounds that each reported 0.05 in the same unit sum to one turn at 0.10.
  REQUIRE(result.usage->cost.has_value());
  REQUIRE(result.usage->cost->amount == Approx(0.10));
  REQUIRE(result.usage->cost->unit == "credits");
}

TEST_CASE("Rebuilding the registry leaves one address the executor can keep reading",
          "[executor][tool_loop]") {
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<NamedTool>("older_tool"));
  const Toolbox* address = &toolbox;

  toolbox.Clear();
  toolbox.RegisterTool(std::make_unique<NamedTool>("newer_tool"));

  REQUIRE(&toolbox == address);
  const auto defs = toolbox.ToolDefinitions();
  REQUIRE(defs.size() == 1);
  REQUIRE(defs.front().name == "newer_tool");
}

TEST_CASE("A registry rejects a tool without a name instead of throwing", "[executor][tool_loop]") {
  Toolbox toolbox;
  REQUIRE(toolbox.RegisterTool(nullptr) == false);
  REQUIRE(toolbox.RegisterTool(std::make_unique<NamedTool>("")) == false);
  REQUIRE(toolbox.ToolDefinitions().empty());
}

TEST_CASE("A rejected registration leaves the registry usable", "[executor][tool_loop]") {
  Toolbox toolbox;
  REQUIRE(toolbox.RegisterTool(std::make_unique<NamedTool>("keeper")) == true);
  REQUIRE(toolbox.RegisterTool(std::make_unique<NamedTool>("")) == false);
  REQUIRE(toolbox.RegisterTool(std::make_unique<NamedTool>("later")) == true);

  const auto defs = toolbox.ToolDefinitions();
  REQUIRE(defs.size() == 2);
}

TEST_CASE("A tool the registry no longer holds cannot be executed", "[executor][tool_loop]") {
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<NamedTool>("older_tool"));
  toolbox.Clear();

  ToolContext ctx;
  const std::string raw = toolbox.ExecuteTool("older_tool", boost::json::object{}, ctx);
  const tools::ToolResult parsed = tools::ParseToolResult(raw);

  REQUIRE(parsed.valid);
  REQUIRE(parsed.success == false);
  REQUIRE(parsed.error == "Tool not found: older_tool");
}

TEST_CASE("The executor sends the system inputs ahead of the stored turns", "[executor][request]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);
  executor.SetSystemPrompt("be brief");

  Conversation ws;

  CapturingLLM provider;
  ExecutionResult result = executor.Execute("hello", ws, &provider, toolbox);

  REQUIRE(result.has_error == false);

  const std::vector<ChatMessage>& sent = provider.captured();
  REQUIRE(sent.size() == 2);

  REQUIRE(sent[0].role == "system");
  REQUIRE(sent[0].content.find("be brief") == 0);
  REQUIRE(sent[0].content.find("=== Environment ===") != std::string::npos);

  REQUIRE(sent[1].role == "user");
  REQUIRE(sent[1].content == "hello");
}

TEST_CASE("The executor sends no prompt of its own", "[executor][request]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  Conversation ws;

  CapturingLLM provider;
  executor.Execute("hello", ws, &provider, toolbox);

  const std::vector<ChatMessage>& sent = provider.captured();
  REQUIRE(sent.size() == 2);
  REQUIRE(sent[0].content.find("=== Environment ===") == 0);
}

TEST_CASE("A failed request is reported and not stored", "[executor][errors]") {
  // The global token decides whether a throw reads as abandoned rather than failed;
  // another test may have left it set, so the case pins it before it runs.
  const bool was_set = platform::ShutdownRequested();
  platform::ShutdownToken()->store(false, std::memory_order_release);

  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  Conversation ws;
  FailingLLM provider;
  const ExecutionResult result = executor.Execute("hello", ws, &provider, toolbox);

  REQUIRE(result.has_error);
  REQUIRE(result.error_message.find("maximum context length") != std::string::npos);

  const std::vector<ChatMessage> history = ws.History();
  REQUIRE(history.size() == 1);
  REQUIRE(history[0].role == "user");
  REQUIRE(history[0].content == "hello");

  platform::ShutdownToken()->store(was_set, std::memory_order_release);
}

TEST_CASE("A stop the caller asked for is not reported as a failure", "[executor][tool_loop]") {
  platform::ShutdownToken()->store(false, std::memory_order_release);

  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  FailingLLM provider;
  const CancelToken withdrawn = std::make_shared<std::atomic<bool>>(true);

  Conversation ws;
  const ExecutionResult result = executor.Execute("ask", ws, &provider, toolbox, withdrawn);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content.empty());

  REQUIRE(ws.History().size() == 1);
  REQUIRE(ws.History()[0].role == "user");
}

TEST_CASE("A shutdown abandons the turn without calling it a cancel", "[executor][tool_loop]") {
  platform::ShutdownToken()->store(false, std::memory_order_release);

  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  FailingLLM provider;

  Conversation ws;
  platform::ShutdownToken()->store(true, std::memory_order_release);
  // A failed REQUIRE unwinds past the reset below; restore on the way out either way,
  // or every later test in this binary runs inside a shutdown that never happened.
  struct Reset {
    ~Reset() { platform::ShutdownToken()->store(false, std::memory_order_release); }
  } reset;
  (void)reset;

  const ExecutionResult result = executor.Execute("ask", ws, &provider, toolbox);

  REQUIRE(result.has_error == false);
  REQUIRE(result.outcome == TurnOutcome::kCompleted);
  REQUIRE(ws.History().size() == 1);
  REQUIRE(ws.History()[0].role == "user");
}

TEST_CASE("A reply stopped at the token limit is reported as incomplete", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("half a sentence", "length");
  Conversation ws;
  const ExecutionResult result = executor.Execute("write a lot", ws, &provider, toolbox);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "half a sentence");
  REQUIRE_FALSE(result.notice.empty());
  REQUIRE(ws.History().size() == 2);
}

TEST_CASE("A reply the model ended itself carries no remark", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("a whole answer", "stop");
  Conversation ws;
  const ExecutionResult result = executor.Execute("ask", ws, &provider, toolbox);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "a whole answer");
  REQUIRE(result.notice.empty());
}

TEST_CASE("A tool call without a name still gets an answer in the store", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  ToolCall call;
  call.id = "call_1";
  call.name = "";  // the provider named no tool
  call.arguments = boost::json::object{};

  MockLLM provider(std::vector<ToolCall>{call}, "done", /*fire_calls_once=*/true);

  Conversation ws;
  const ExecutionResult result = executor.Execute("go", ws, &provider, toolbox);

  REQUIRE(result.has_error == false);

  bool stored_call = false;
  bool stored_answer = false;
  for (const auto& msg : ws.History()) {
    if (msg.role == "assistant" && msg.HasToolCalls()) {
      for (const auto& tc : msg.tool_calls.as_array()) {
        if (tc.at("id") == "call_1") stored_call = true;
      }
    }
    if (msg.role == "tool" && msg.tool_call_id == "call_1") stored_answer = true;
  }
  REQUIRE(stored_call);
  REQUIRE(stored_answer);
}

TEST_CASE("The model that answered is carried out of the turn", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("hi", "stop", "gpt-4o-mini-2024-07-18");
  Conversation ws;
  const ExecutionResult result = executor.Execute("ask", ws, &provider, toolbox);

  REQUIRE(result.model == "gpt-4o-mini-2024-07-18");
}

TEST_CASE("Reasoning reaches the caller as it is produced", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("answer", "stop", "", "weighing the options");
  std::string streamed;
  Conversation ws;
  const ExecutionResult result =
      executor.Execute("think", ws, &provider, toolbox, nullptr, nullptr, {},
                       [&](std::string_view token) { streamed += token; });

  REQUIRE(streamed == "weighing the options");
  REQUIRE(result.content == "answer");
}

TEST_CASE("A note raised mid-turn lands after every receipt of the round",
          "[executor][tool_loop][note]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  auto noting = std::make_unique<NotingTool>(&executor, "stopped: wrong file");
  toolbox.RegisterTool(std::move(noting));

  TwoCallLLM provider;
  Conversation ws;
  // The note rides into the store only when the turn is cancellable, which the served turn
  // always is; the CLI passes one too.
  const CancelToken live = std::make_shared<std::atomic<bool>>(false);
  const ExecutionResult result = executor.Execute("go", ws, &provider, toolbox, live);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "done");

  // The second round's request view: the note must sit after both receipts, never
  // between them - a user message splitting the assistant's tool_calls from their
  // receipts is what the gateway answers with a 400. The view leads with the
  // environment context, so the turn's own messages start at index 1.
  const std::vector<ChatMessage>& messages = provider.second_round();
  REQUIRE(messages.size() == 6);
  REQUIRE(messages[1].role == "user");
  REQUIRE(messages[2].role == "assistant");
  REQUIRE(messages[2].HasToolCalls());
  REQUIRE(messages[3].role == "tool");
  REQUIRE(messages[3].tool_call_id == "call_1");
  REQUIRE(messages[4].role == "tool");
  REQUIRE(messages[4].tool_call_id == "call_2");
  REQUIRE(messages[5].role == "user");
  REQUIRE(messages[5].content.find("stopped: wrong file") != std::string::npos);

  // The store keeps the same order, so a saved session cannot carry the split either.
  // The final round's answer is stored after the note, hence six entries.
  const std::vector<ChatMessage> stored = ws.History();
  REQUIRE(stored.size() == 6);
  REQUIRE(stored[0].role == "user");
  REQUIRE(stored[1].role == "assistant");
  REQUIRE(stored[2].role == "tool");
  REQUIRE(stored[3].role == "tool");
  REQUIRE(stored[4].role == "user");
  REQUIRE(stored[4].content.find("stopped: wrong file") != std::string::npos);
  REQUIRE(stored[5].role == "assistant");
  REQUIRE(stored[5].content == "done");
}

// A tool that answers the way the sandbox does: the command never ran, and the result says
// why. `refusal` picks which kind of refusal the turn has to tell apart.
class RefusingTool : public Tool {
 public:
  explicit RefusingTool(tools::Refusal refusal) : refusal_(refusal) {}

  std::string Name() const override { return "execute_bash"; }
  std::string Description() const override { return "refuses what the policy names"; }
  boost::json::value ParametersSchema() const override {
    return boost::json::object{{"type", "object"}};
  }
  std::string Execute(const boost::json::value& /*args*/, ToolContext& /*ctx*/) override {
    return tools::MakeToolResultJson(false, "", "refused before it ran", -1, true, refusal_);
  }

 private:
  tools::Refusal refusal_;
};

namespace {

// One tool call, answered by the model once, then a plain reply: the shape of a turn whose
// only work was a refused command.
std::vector<ToolCall> OneCallTo(const std::string& name) {
  ToolCall call;
  call.id = "call_1";
  call.name = name;
  call.arguments = boost::json::object{{"command", "sudo rm -rf /"}};
  return {call};
}

}  // namespace

TEST_CASE("A turn every one of whose calls the sandbox refused says so", "[executor][refusal]") {
  Executor executor;
  Conversation conversation;
  // The model asks for the tool in the first round and answers nothing in the second, so the
  // refusal is the whole turn.
  MockLLM provider(OneCallTo("execute_bash"), /*content=*/"", /*fire_calls_once=*/true);
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<RefusingTool>(tools::Refusal::kPolicy));

  const ExecutionResult result =
      executor.Execute("try it", conversation, &provider, toolbox);

  REQUIRE(result.blocked_tool_calls == 1);
  REQUIRE(result.unaskable_tool_calls == 0);
  REQUIRE(result.only_blocked_calls);
  REQUIRE_FALSE(result.only_unaskable_calls);
  REQUIRE(result.has_error);
}

TEST_CASE("A turn refused only for want of an approval channel is a different answer",
          "[executor][refusal]") {
  Executor executor;
  Conversation conversation;
  MockLLM provider(OneCallTo("execute_bash"), "", true);
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<RefusingTool>(tools::Refusal::kNobodyToAsk));

  const ExecutionResult result =
      executor.Execute("try it", conversation, &provider, toolbox);

  // Both kinds are refusals, and both stop the turn: what differs is whose problem it is.
  REQUIRE(result.blocked_tool_calls == 1);
  REQUIRE(result.unaskable_tool_calls == 1);
  REQUIRE(result.only_blocked_calls);
  REQUIRE(result.only_unaskable_calls);
  REQUIRE(result.error_message.find("nothing here can ask") != std::string::npos);
}

TEST_CASE("A refusal the turn recovered from is not the turn's outcome",
          "[executor][refusal]") {
  Executor executor;
  Conversation conversation;
  MockLLM provider(OneCallTo("execute_bash"), "answered anyway", /*fire_calls_once=*/true);
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<RefusingTool>(tools::Refusal::kPolicy));

  const ExecutionResult result =
      executor.Execute("try it", conversation, &provider, toolbox);

  // The refusal is counted, because it happened, and it is not the outcome: the model read
  // it and still produced an answer.
  REQUIRE(result.blocked_tool_calls == 1);
  REQUIRE_FALSE(result.only_blocked_calls);
  REQUIRE_FALSE(result.has_error);
  REQUIRE(result.content == "answered anyway");
}
