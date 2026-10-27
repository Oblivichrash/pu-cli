// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/projection.hpp"
#include <catch2/catch_test_macros.hpp>

#include "pu/session/request.hpp"

#include <boost/json.hpp>

#include <string>
#include <vector>

using namespace pu;

namespace {

session::MessageId AppendUser(session::MessageGraph& graph, const std::string& text) {
  session::UserPayload user;
  user.content = text;
  return graph.AppendAfterLeaf(std::move(user)).id;
}

session::MessagePayload Assistant(const std::string& text) {
  session::AssistantPayload assistant;
  assistant.content = text;
  return assistant;
}

}  // namespace

TEST_CASE("The system inputs come before the stored turns", "[request]") {
  session::MessageGraph graph;
  AppendUser(graph, "hello");
  graph.AppendAfterLeaf(Assistant("hi"));

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";
  inputs.environment = "=== Environment ===";

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[0].content == "be brief\n\n=== Environment ===");
  REQUIRE(messages[1].role == "user");
  REQUIRE(messages[1].content == "hello");
  REQUIRE(messages[2].role == "assistant");
  REQUIRE(messages[2].content == "hi");
}

TEST_CASE("Each system input is optional", "[request]") {
  session::MessageGraph graph;
  AppendUser(graph, "hello");

  session::RequestInputs prompt_only;
  prompt_only.system_prompt = "be brief";
  auto messages = session::BuildRequestPath(graph, prompt_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "be brief");

  session::RequestInputs environment_only;
  environment_only.environment = "=== Environment ===";
  messages = session::BuildRequestPath(graph, environment_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "=== Environment ===");

  messages = session::BuildRequestPath(graph, session::RequestInputs{});
  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "user");
}

TEST_CASE("An empty conversation still carries its system inputs", "[request]") {
  const session::MessageGraph graph;

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[0].content == "be brief");
}

TEST_CASE("A stored system node is passed through", "[request]") {
  session::MessageGraph graph;
  AppendUser(graph, "one");

  session::SystemPayload note;
  note.content = "a note from the caller";
  graph.AppendAfterLeaf(std::move(note));

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].content == "be brief");
  REQUIRE(messages[1].content == "one");
  REQUIRE(messages[2].content == "a note from the caller");
}

TEST_CASE("Tool calls and receipts survive the request view", "[request]") {
  session::MessageGraph graph;
  session::AssistantPayload assistant;
  assistant.content = "checking";
  assistant.reasoning.push_back(session::Reasoning{"because", {}, nullptr});
  assistant.tool_calls.push_back(
      session::ToolCallRecord{"call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  session::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content = "file.txt";
  graph.AppendAfterLeaf(std::move(receipt));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].HasToolCalls());
  REQUIRE(llm::ProjectReasoningText(messages[0].reasoning) == "because");
  REQUIRE(messages[0].tool_calls.as_array()[0].at("function").at("arguments").at("path") == ".");
  REQUIRE(messages[1].role == "tool");
  REQUIRE(messages[1].tool_name == "ls");
  REQUIRE(messages[1].tool_call_id == "call_1");
}

TEST_CASE("Rendered positions follow the view, not the stored ids", "[request]") {
  session::MessageGraph graph;
  AppendUser(graph, "one");
  graph.AppendAfterLeaf(Assistant("two"));
  AppendUser(graph, "three");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages[0].id == 1);
  REQUIRE(messages[1].id == 2);
  REQUIRE(messages[2].id == 3);
}

TEST_CASE("Every stored node reaches the model, with its tool receipt", "[request]") {
  session::MessageGraph graph;
  for (int i = 1; i <= 20; ++i) AppendUser(graph, "msg" + std::to_string(i));

  session::AssistantPayload assistant;
  assistant.tool_calls.push_back(
      session::ToolCallRecord{"call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  session::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content = "file.txt";
  graph.AppendAfterLeaf(std::move(receipt));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages.size() == graph.Size());
  REQUIRE(messages.size() == 22);

  bool call_followed_by_receipt = false;
  for (std::size_t i = 0; i + 1 < messages.size(); ++i) {
    if (messages[i].HasToolCalls() && messages[i + 1].role == "tool" &&
        messages[i + 1].tool_call_id == "call_1") {
      call_followed_by_receipt = true;
    }
  }
  REQUIRE(call_followed_by_receipt);
}

namespace {

// A page as execute_bash writes one: the envelope, plus the two ids the projection
// reads.
std::string PageJson(const std::string& source, const std::string& buffer,
                     const std::string& text) {
  // The envelope a page really carries: where the page sits, which run produced it, how
  // much of that run survived.
  boost::json::object page{{"first", 1}, {"last", 3}, {"earlier", nullptr}, {"later", nullptr}};
  boost::json::object result{{"success", true},
                             {"blocked", false},
                             {"output", text},
                             {"error", ""},
                             {"exit_code", 0},
                             {"page", page},
                             {"buffer", boost::json::object{{"id", buffer}, {"source", source}}},
                             {"extent", boost::json::object{{"lines", 3}, {"retained", 3}, {"truncated", false}}}};
  return boost::json::serialize(result);
}

void AppendPage(session::MessageGraph& graph, const std::string& source, const std::string& buffer,
                const std::string& text) {
  session::ToolPayload receipt;
  receipt.tool_call_id = "call";
  receipt.tool_name = "execute_bash";
  receipt.content = PageJson(source, buffer, text);
  graph.AppendAfterLeaf(std::move(receipt));
}

}  // namespace

TEST_CASE("A page of an older run of one command is replaced by a receipt", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "line 1 of run one\n");
  AppendPage(graph, "s1", "o2", "line 1 of run two\n");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages.size() == 2);

  const boost::json::value older = boost::json::parse(messages[0].content);
  REQUIRE(boost::json::value_to<std::string>(older.at("output")).empty());
  REQUIRE(boost::json::value_to<std::string>(older.at("superseded_by")) == "o2");
  // The page and the extent survive, so the model still knows how much it is not seeing.
  REQUIRE(older.at("extent").at("lines") == 3);
  REQUIRE(older.at("page").at("first") == 1);

  REQUIRE(messages[1].content.find("line 1 of run two") != std::string::npos);
}

TEST_CASE("Two pages of one run are both kept", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "first page\n");
  AppendPage(graph, "s1", "o1", "second page\n");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages.size() == 2);
  // Complementary, not successive: dropping the first would take away what the model
  // read on the way to the second.
  REQUIRE(messages[0].content.find("first page") != std::string::npos);
  REQUIRE(messages[1].content.find("second page") != std::string::npos);
}

TEST_CASE("Pages of different commands do not replace each other", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "from ls\n");
  AppendPage(graph, "s2", "o2", "from cat\n");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages[0].content.find("from ls") != std::string::npos);
  REQUIRE(messages[1].content.find("from cat") != std::string::npos);
}

TEST_CASE("A tool result that is no page is left alone", "[request]") {
  session::MessageGraph graph;
  session::ToolPayload plain;
  plain.tool_call_id = "call";
  plain.tool_name = "write_file";
  plain.content = R"({"success":true,"output":"","error":"","exit_code":0})";
  graph.AppendAfterLeaf(std::move(plain));
  AppendPage(graph, "s1", "o1", "from ls\n");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages[0].content.find("\"success\":true") != std::string::npos);
  REQUIRE(messages[0].content.find("superseded_by") == std::string::npos);
  REQUIRE(messages[1].content.find("from ls") != std::string::npos);
}

TEST_CASE("A page read after a newer run is not replaced", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "run one\n");
  AppendPage(graph, "s1", "o2", "run two\n");
  // The model paged back into the older run on purpose; eliding what it just asked for
  // would only make it ask again.
  AppendPage(graph, "s1", "o1", "run one, page two\n");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].content.find("superseded_by") != std::string::npos);
  REQUIRE(messages[1].content.find("run two") != std::string::npos);
  REQUIRE(messages[2].content.find("run one, page two") != std::string::npos);
}
