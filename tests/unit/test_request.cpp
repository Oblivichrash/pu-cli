// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/projection.hpp"
#include <catch2/catch_test_macros.hpp>

#include "pu/exec/run.hpp"
#include "pu/paging/paging.hpp"
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

// A page as execute_bash writes one: the envelope the pager itself writes, so this fixture
// cannot drift from the writer the projection reads.
std::string PageJson(const std::string& source, const std::string& buffer,
                     const std::string& text, std::size_t rank) {
  exec::Verdict verdict;
  verdict.command = "seq 1 3";
  verdict.ok = true;

  exec::Extent extent;
  extent.lines = 3;
  extent.bytes = text.size();
  extent.retained = 3;

  const paging::Buffer run(buffer, source, verdict, text, extent, rank);
  const paging::Page page{text, 1, 3, std::nullopt, std::nullopt};

  boost::json::object result{{"success", true},
                             {"blocked", false},
                             {"output", text},
                             {"error", ""},
                             {"exit_code", 0}};
  for (const auto& field : paging::Envelope(run, page)) result[field.key()] = field.value();
  return boost::json::serialize(result);
}

// `rank` is the order the runs were *started*, which is what the projection compares: two
// pages of one run share it, and a later run of the same question outranks both.
void AppendPage(session::MessageGraph& graph, const std::string& source, const std::string& buffer,
                const std::string& text, std::size_t rank) {
  session::ToolPayload receipt;
  receipt.tool_call_id = "call";
  receipt.tool_name = "execute_bash";
  receipt.content = PageJson(source, buffer, text, rank);
  // Set here rather than derived from the text: the projection reads what the writer stored,
  // and what the writer stores is the subject of test_conversation.cpp.
  receipt.origin = paging::Origin{source, buffer, rank};
  graph.AppendAfterLeaf(std::move(receipt));
}

}  // namespace

TEST_CASE("A page of an older run of one command is replaced by a receipt", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "line 1 of run one\n", 1);
  AppendPage(graph, "s1", "o2", "line 1 of run two\n", 2);

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
  AppendPage(graph, "s1", "o1", "first page\n", 1);
  AppendPage(graph, "s1", "o1", "second page\n", 1);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages.size() == 2);
  // Complementary, not successive: dropping the first would take away what the model
  // read on the way to the second.
  REQUIRE(messages[0].content.find("first page") != std::string::npos);
  REQUIRE(messages[1].content.find("second page") != std::string::npos);
}

TEST_CASE("Pages of different commands do not replace each other", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "from ls\n", 1);
  AppendPage(graph, "s2", "o2", "from cat\n", 2);

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
  AppendPage(graph, "s1", "o1", "from ls\n", 1);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages[0].content.find("\"success\":true") != std::string::npos);
  REQUIRE(messages[0].content.find("superseded_by") == std::string::npos);
  REQUIRE(messages[1].content.find("from ls") != std::string::npos);
}

TEST_CASE("A page read after a newer run is not replaced", "[request]") {
  session::MessageGraph graph;
  AppendPage(graph, "s1", "o1", "run one\n", 1);
  AppendPage(graph, "s1", "o2", "run two\n", 2);
  // The model paged back into the older run on purpose; eliding what it just asked for
  // would only make it ask again.
  AppendPage(graph, "s1", "o1", "run one, page two\n", 1);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].content.find("superseded_by") != std::string::npos);
  REQUIRE(messages[1].content.find("run two") != std::string::npos);
  REQUIRE(messages[2].content.find("run one, page two") != std::string::npos);
}

TEST_CASE("A tombstone keeps the verdict and drops the way to ask for more", "[request]") {
  const std::string page = PageJson("s1", "o1", "line 1 of run one\nline 2\nline 3\n", 1);
  const std::string tombstone = paging::TombstonedReceipt(page);

  const boost::json::value kept = boost::json::parse(tombstone);
  // What happened survives: a model that forgets a command failed will run it again.
  REQUIRE(boost::json::value_to<bool>(kept.at("success")));
  // The envelope a page is written as carries no verdict -- the command lives in the
  // conversation, not in the result -- so a tombstone keeps the parts a page does have:
  // which run it was and how much of it there was.
  REQUIRE(kept.at("extent").at("lines") == 3);
  REQUIRE(boost::json::value_to<std::string>(kept.at("buffer").at("id")) == "o1");
  REQUIRE(boost::json::value_to<std::string>(kept.at("buffer").at("source")) == "s1");
  // How to ask for it again does not: a cursor is a standing invitation to spend a turn.
  REQUIRE(!kept.as_object().contains("page"));
  REQUIRE(!kept.as_object().contains("output"));
  // And the loss is said, not implied: a silently empty body reads as a command that
  // printed nothing.
  REQUIRE(boost::json::value_to<std::string>(kept.at("elided")) == "tombstone");
  // The origin still resolves, and that is the point of keeping `buffer`: the model can tell
  // which run it is looking at, even though the page number it would have used is gone.
  REQUIRE(paging::OriginOf(tombstone).has_value());
  REQUIRE(tombstone.size() < page.size());
}

TEST_CASE("A tombstone of something that is not an envelope is left alone", "[request]") {
  // A refusal, an empty result, a body a tool wrote itself: there is no verdict inside to
  // lift out, and inventing one would put a success the call never had in front of the model.
  for (const std::string& body : {std::string("plain text"), std::string(""),
                                  std::string("[]")}) {
    REQUIRE(paging::TombstonedReceipt(body) == body);
  }
  // A refusal is an object, and it is one of the things a tombstone exists to preserve: the
  // `refusal` field survives the rewrite along with the verdict beside it.
  const boost::json::value refusal =
      boost::json::parse(paging::TombstonedReceipt(R"({"success":false,"refusal":"no"})"));
  REQUIRE(boost::json::value_to<std::string>(refusal.at("refusal")) == "no");
  REQUIRE(!boost::json::value_to<bool>(refusal.at("success")));
}

namespace {

// One assistant turn that calls a tool, and the receipt that answers it. Adding these in
// pairs is what the budget must never break apart.
//
// Each pair names its own `source`, so no page supersedes another and the budget is the only
// thing acting on the results. Sharing one command would make the supersession rule elide
// most of them first, and the test would pass without exercising the budget at all.
void AppendCallAndReceipt(session::MessageGraph& graph, int n, const std::string& output) {
  session::AssistantPayload assistant;
  assistant.content = "calling";
  const std::string id = "call_" + std::to_string(n);
  assistant.tool_calls.push_back(session::ToolCallRecord{id, "execute_bash", boost::json::parse(
                                                                               R"({"command":"seq"})")});
  graph.AppendAfterLeaf(std::move(assistant));

  const std::string source = "s" + std::to_string(n);
  const std::string buffer = "o" + std::to_string(n);
  session::ToolPayload receipt;
  receipt.tool_call_id = id;
  receipt.tool_name = "execute_bash";
  receipt.content = PageJson(source, buffer, output, static_cast<std::size_t>(n));
  receipt.origin = paging::Origin{source, buffer, static_cast<std::size_t>(n)};
  graph.AppendAfterLeaf(std::move(receipt));
}

std::string Wide(int lines) {
  std::string text;
  for (int i = 1; i <= lines; ++i) text += "line " + std::to_string(i) + " of the output\n";
  return text;
}

}  // namespace

TEST_CASE("A request with no budget is the request it would be without one", "[request]") {
  session::MessageGraph graph;
  for (int i = 1; i <= 10; ++i) AppendCallAndReceipt(graph, i, Wide(40));

  const std::vector<ChatMessage> unbounded =
      session::BuildRequestPath(graph, session::RequestInputs{"prompt", "env", 0});
  // One system message, then every node: a budget of zero is not a budget.
  REQUIRE(unbounded.size() == graph.Size() + 1);
  REQUIRE(unbounded.front().role == "system");
}

TEST_CASE("A tight budget tombstones the oldest results and leaves the pairs intact",
          "[request]") {
  session::MessageGraph graph;
  for (int i = 1; i <= 12; ++i) AppendCallAndReceipt(graph, i, Wide(40));

  session::RequestInputs inputs;
  inputs.tool_bytes = 12000;
  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  // Nothing is removed, so every call still has the receipt that names it. This is the one
  // invariant a projection may not break: a `tool` message whose `tool_call_id` matches no
  // call is rejected by every provider.
  REQUIRE(messages.size() == graph.Size());
  for (const ChatMessage& msg : messages) {
    if (msg.role != "tool") continue;
    bool answered = false;
    for (const ChatMessage& call : messages) {
      if (!call.HasToolCalls()) continue;
      for (const auto& tc : call.tool_calls.as_array()) {
        if (boost::json::value_to<std::string>(tc.at("id")) == msg.tool_call_id) answered = true;
      }
    }
    REQUIRE(answered);
  }


  std::size_t tool_total = 0;
  int tombstones = 0;
  for (const ChatMessage& msg : messages) {
    if (msg.role != "tool") continue;
    tool_total += msg.content.size();
    if (msg.content.find("\"elided\":\"tombstone\"") != std::string::npos) ++tombstones;
  }
  // The budget is spent from the oldest end and stops as soon as it fits, so it comes out
  // near the limit rather than far under it, and something really was compressed.
  REQUIRE(tombstones > 0);
  REQUIRE(tool_total <= 12000);
  // Only as many as it took: the results near the end are kept whole because the budget
  // allowed it, not because they were protected.
  REQUIRE(tombstones < 12);

  // The newest results are whole: they are what the current turn is reasoning about.
  const std::string& newest = messages.back().content;
  REQUIRE(newest.find("line 40 of the output") != std::string::npos);
  REQUIRE(newest.find("tombstone") == std::string::npos);
}

TEST_CASE("A budget the results already fit inside changes nothing", "[request]") {
  session::MessageGraph graph;
  for (int i = 1; i <= 4; ++i) AppendCallAndReceipt(graph, i, Wide(3));

  session::RequestInputs inputs;
  inputs.tool_bytes = 1u << 20;
  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);
  for (const ChatMessage& msg : messages) REQUIRE(msg.content.find("tombstone") == std::string::npos);
}

TEST_CASE("A budget never tombstones a result of the newest few turns", "[request]") {
  session::MessageGraph graph;
  for (int i = 1; i <= 8; ++i) AppendCallAndReceipt(graph, i, Wide(40));

  session::RequestInputs inputs;
  inputs.tool_bytes = 1;  // impossible to satisfy; the hold floor is what answers this
  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  std::vector<std::size_t> tool_at;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (messages[i].role == "tool") tool_at.push_back(i);
  }
  REQUIRE(tool_at.size() == 8);

  // The newest results are held regardless of the budget, because compressing the output the
  // current turn just asked for would make the model ask for it again -- a saved token that
  // costs a turn.
  for (std::size_t k = tool_at.size() - 6; k < tool_at.size(); ++k) {
    REQUIRE(messages[tool_at[k]].content.find("tombstone") == std::string::npos);
  }
  // Everything older than the newest six is fair game, and a budget that cannot be met spends
  // all of it: the oldest result is a tombstone, not the text it started as.
  const std::string& oldest = messages[tool_at.front()].content;
  REQUIRE(oldest.find("\"elided\":\"tombstone\"") != std::string::npos);
  REQUIRE(oldest.find("line 40 of the output") == std::string::npos);
}
