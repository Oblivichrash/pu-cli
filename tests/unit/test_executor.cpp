// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"
#include "pu/core/base.hpp"
#include "pu/core/platform.hpp"
#include "pu/tools/builtin_tools.hpp"

#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("BuildStaticSystemContext includes security policy when set", "[executor]") {
  Executor executor;
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
  REQUIRE(msg.find("head -n 50") != std::string::npos);
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

  ChatResult Chat(const std::vector<ChatMessage>& /*history*/,
                  const std::vector<ToolDefinition>& /*tools*/,
                  std::function<void(const std::string&)> /*content_callback*/,
                  CancelToken /*cancel_token*/,
                  std::function<void(const std::string&)> /*reasoning_callback*/) override {
    ChatResult r;
    r.content = content_;
    r.tool_calls = calls_;
    if (fire_calls_once_) calls_.clear();
    return r;
  }

  bool SupportsTools() const override { return true; }

 private:
  std::vector<ToolCall> calls_;
  std::string content_;
  bool fire_calls_once_ = false;
};

class OfferingLLM : public LLMProvider {
 public:
  OfferingLLM(std::string wanted, std::string content = "")
      : wanted_(std::move(wanted)), content_(std::move(content)) {}

  ChatResult Chat(const std::vector<ChatMessage>& /*history*/,
                  const std::vector<ToolDefinition>& tools,
                  std::function<void(const std::string&)> /*content_callback*/,
                  CancelToken /*cancel_token*/,
                  std::function<void(const std::string&)> /*reasoning_callback*/) override {
    ChatResult r;
    r.content = content_;

    offered.clear();
    bool wanted_is_offered = false;
    for (const ToolDefinition& def : tools) {
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

  bool SupportsTools() const override { return true; }

  std::vector<std::string> offered;

 private:
  std::string wanted_;
  std::string content_;
  bool asked_ = false;
};

class FailingLLM : public LLMProvider {
 public:
  ChatResult Chat(const std::vector<ChatMessage>& /*history*/,
                  const std::vector<ToolDefinition>& /*tools*/,
                  std::function<void(const std::string&)> /*content_callback*/,
                  CancelToken /*cancel_token*/,
                  std::function<void(const std::string&)> /*reasoning_callback*/) override {
    throw pu::HttpError("HTTP error 400: maximum context length is 4096 tokens");
  }

  bool SupportsTools() const override { return true; }
};

class CapturingLLM : public LLMProvider {
 public:
  ChatResult Chat(const std::vector<ChatMessage>& history,
                  const std::vector<ToolDefinition>& /*tools*/,
                  std::function<void(const std::string&)> /*content_callback*/,
                  CancelToken /*cancel_token*/,
                  std::function<void(const std::string&)> /*reasoning_callback*/) override {
    history_ = history;
    ChatResult r;
    r.content = "done";
    return r;
  }

  bool SupportsTools() const override { return true; }

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

  ChatResult Chat(const std::vector<ChatMessage>& /*history*/,
                  const std::vector<ToolDefinition>& /*tools*/,
                  std::function<void(const std::string&)> /*content_callback*/,
                  CancelToken /*cancel_token*/,
                  std::function<void(const std::string&)> reasoning_callback) override {
    if (reasoning_callback && !reasoning_.empty()) reasoning_callback(reasoning_);
    ChatResult r;
    r.content = content_;
    r.finish_reason = finish_reason_;
    r.model = model_;
    r.reasoning_content = reasoning_;
    return r;
  }

  bool SupportsTools() const override { return true; }

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
    return R"({"success":true,"stdout":"ran","stderr":"","error":"","exit_code":0})";
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
    return R"({"success":true,"stdout":"","stderr":"","error":"","exit_code":0})";
  }

  int executions = 0;

 private:
  std::string name_;
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
  std::vector<std::string> ended_outputs;
  std::vector<std::string> ended_errors;

  ToolCallbacks cb;
  cb.on_start = [&](const std::string& id, const std::string& name,
                    const boost::json::value& args) {
    started_ids.push_back(id);
    started_names.push_back(name);
    started_args.push_back(args);
  };
  cb.on_end = [&](const std::string& id, const std::string& output, const std::string& error) {
    ended_ids.push_back(id);
    ended_outputs.push_back(output);
    ended_errors.push_back(error);
  };

  Conversation ws;
  ExecutionResult result = executor.Execute("run it", ws, &mock, &toolbox, nullptr, nullptr, cb);

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

  REQUIRE(ended_outputs[0] == "ran");
  REQUIRE(ended_errors[0].empty());

  bool found_paired_tool_msg = false;
  for (const auto& msg : ws.GetHistory()) {
    if (msg.role == "tool" && msg.tool_call_id == started_ids[0]) {
      found_paired_tool_msg = true;
      break;
    }
  }
  REQUIRE(found_paired_tool_msg);
}

TEST_CASE("Each turn reads the toolbox it was given, not one held from an earlier turn",
          "[executor][tool_loop]") {
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  auto older = std::make_unique<NamedTool>("older_tool");
  Toolbox first;
  first.RegisterTool(std::move(older));

  auto counter = std::make_unique<TrackingTool>("newer_tool");
  auto* counter_ptr = counter.get();
  Toolbox second;
  second.RegisterTool(std::move(counter));

  Conversation ws;
  OfferingLLM provider("newer_tool", "done");

  const ExecutionResult result = executor.Execute("run it", ws, &provider, &second);

  REQUIRE(result.has_error == false);
  REQUIRE(provider.offered == std::vector<std::string>{"newer_tool"});
  REQUIRE(counter_ptr->executions == 1);
}

TEST_CASE("The toolbox a turn reads is the only one it can reach", "[executor][tool_loop]") {
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  auto older = std::make_unique<NamedTool>("older_tool");
  auto* older_ptr = older.get();
  Toolbox first;
  first.RegisterTool(std::move(older));

  auto counter = std::make_unique<TrackingTool>("newer_tool");
  auto* counter_ptr = counter.get();
  Toolbox second;
  second.RegisterTool(std::move(counter));

  Conversation ws;
  OfferingLLM provider("older_tool", "done");

  const ExecutionResult result = executor.Execute("run it", ws, &provider, &second);

  REQUIRE(result.has_error == false);
  REQUIRE(provider.offered == std::vector<std::string>{"newer_tool"});
  REQUIRE(older_ptr->executions == 0);
  REQUIRE(counter_ptr->executions == 0);
}

TEST_CASE("A turn with no toolbox reports that it cannot run rather than reading through it",
          "[executor][tool_loop]") {
  Conversation ws;
  MockLLM provider({}, "unused");
  Executor executor;

  const ExecutionResult result = executor.Execute("run it", ws, &provider, nullptr);

  REQUIRE(result.has_error);
  REQUIRE(result.error_message == "Tool registry is not initialized.");
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
  ExecutionResult result = executor.Execute("hello", ws, &provider, &toolbox);

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
  executor.Execute("hello", ws, &provider, &toolbox);

  const std::vector<ChatMessage>& sent = provider.captured();
  REQUIRE(sent.size() == 2);
  REQUIRE(sent[0].content.find("=== Environment ===") == 0);
}

TEST_CASE("A failed request is reported and not stored", "[executor][errors]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  Conversation ws;
  FailingLLM provider;
  const ExecutionResult result = executor.Execute("hello", ws, &provider, &toolbox);

  REQUIRE(result.has_error);
  REQUIRE(result.error_message.find("maximum context length") != std::string::npos);

  const std::vector<ChatMessage> history = ws.GetHistory();
  REQUIRE(history.size() == 1);
  REQUIRE(history[0].role == "user");
  REQUIRE(history[0].content == "hello");
}

TEST_CASE("A stop the caller asked for is not reported as a failure", "[executor][tool_loop]") {
  platform::ClearInterruptFlag();

  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  FailingLLM provider;
  const CancelToken withdrawn = std::make_shared<std::atomic<bool>>(true);

  Conversation ws;
  const ExecutionResult result = executor.Execute("ask", ws, &provider, &toolbox, withdrawn);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content.empty());

  REQUIRE(ws.GetHistory().size() == 1);
  REQUIRE(ws.GetHistory()[0].role == "user");
}

TEST_CASE("A reply stopped at the token limit is reported as incomplete", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("half a sentence", "length");
  Conversation ws;
  const ExecutionResult result = executor.Execute("write a lot", ws, &provider, &toolbox);

  REQUIRE(result.has_error == false);
  REQUIRE(result.content == "half a sentence");
  REQUIRE_FALSE(result.notice.empty());
  REQUIRE(ws.GetHistory().size() == 2);
}

TEST_CASE("A reply the model ended itself carries no remark", "[executor][tool_loop]") {
  Toolbox toolbox;
  Executor executor;
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  StoppingLLM provider("a whole answer", "stop");
  Conversation ws;
  const ExecutionResult result = executor.Execute("ask", ws, &provider, &toolbox);

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
  const ExecutionResult result = executor.Execute("go", ws, &provider, &toolbox);

  REQUIRE(result.has_error == false);

  bool stored_call = false;
  bool stored_answer = false;
  for (const auto& msg : ws.GetHistory()) {
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
  const ExecutionResult result = executor.Execute("ask", ws, &provider, &toolbox);

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
      executor.Execute("think", ws, &provider, &toolbox, nullptr, nullptr, {},
                       [&](const std::string& token) { streamed += token; });

  REQUIRE(streamed == "weighing the options");
  REQUIRE(result.content == "answer");
}
