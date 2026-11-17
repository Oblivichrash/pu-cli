// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/projection.hpp"
#include <catch2/catch_test_macros.hpp>

#include "pu/core/process.hpp"
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

// The smallest unit the projection may handle is a pair: the call and the receipt that answers
// it. A `tool` message whose `tool_call_id` names no call is rejected by every provider, and a
// call with no receipt leaves the model waiting for a result that never arrives. Every tier is
// free to compress or remove whole pairs, so this invariant is checked from both directions
// wherever a tier that removes something is exercised.
void AssertPairsIntact(const std::vector<ChatMessage>& messages) {
  for (const ChatMessage& msg : messages) {
    if (msg.role != "tool") continue;
    bool answered = false;
    for (const ChatMessage& call : messages) {
      if (!call.HasToolCalls()) continue;
      for (const auto& tc : call.tool_calls.as_array()) {
        if (boost::json::value_to<std::string>(tc.at("id")) == msg.tool_call_id) answered = true;
      }
    }
    INFO("orphaned receipt: " << msg.tool_call_id);
    REQUIRE(answered);
  }
  for (const ChatMessage& call : messages) {
    if (!call.HasToolCalls()) continue;
    for (const auto& tc : call.tool_calls.as_array()) {
      const std::string id = boost::json::value_to<std::string>(tc.at("id"));
      bool answered = false;
      for (const ChatMessage& msg : messages) {
        if (msg.role == "tool" && msg.tool_call_id == id) answered = true;
      }
      INFO("orphaned call: " << id);
      REQUIRE(answered);
    }
  }
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

TEST_CASE("A large argument body is elided and a small one is left alone", "[request]") {
  // T4: the argument body is the last pool with no bound. T3 keeps the newest ten pairs, and
  // every one of them re-sends its arguments on each turn -- so a `write_file` of a thousand
  // lines costs its content in tokens forever, while the confirmation it came back with was
  // already reduced to a verdict on the output side.
  session::MessageGraph graph;

  session::AssistantPayload assistant;
  assistant.content = "writing";
  // Two calls in one message: a big `write_file` and a small `bash`. Only the first may change,
  // because eliding a call the model could have re-sent costs a round to recover from.
  const std::string big_content(20000, 'x');
  assistant.tool_calls.push_back(session::ToolCallRecord{
      "call_big", "write_file",
      boost::json::parse(R"({"path":".pu/agents.json","content":""})")});
  assistant.tool_calls[0].arguments.as_object()["content"] = big_content;
  assistant.tool_calls.push_back(
      session::ToolCallRecord{"call_small", "bash", boost::json::parse(R"({"command":"ls"})")});
  graph.AppendAfterLeaf(std::move(assistant));

  for (const char* id : {"call_big", "call_small"}) {
    session::ToolPayload receipt;
    receipt.tool_call_id = id;
    receipt.tool_name = std::string(id) == "call_big" ? "write_file" : "bash";
    receipt.content = "ok";
    graph.AppendAfterLeaf(std::move(receipt));
  }

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  // Ten kilobytes of content becomes a line that says what it was, how much went, and which
  // argument carried it -- while `path` survives, so the model still knows which file it wrote.
  const std::string rendered = boost::json::serialize(messages.front().tool_calls);
  REQUIRE(rendered.find("elided") != std::string::npos);
  REQUIRE(rendered.find(big_content) == std::string::npos);
  REQUIRE(rendered.find(".pu/agents.json") != std::string::npos);
  REQUIRE(rendered.find("\"omitted\":\"content\"") != std::string::npos);

  const boost::json::object& big = messages.front().tool_calls.as_array()[0].as_object();
  const boost::json::object& body = big.at("function").as_object().at("arguments").as_object();
  // One line of content is still one line, whatever its length: the count is what a reader
  // uses to judge whether the body was large, and a byte figure alone would not say that.
  REQUIRE(body.at("lines").as_uint64() == 1);

  // The small call is untouched: it is under the threshold, so nothing about it changes.
  const auto& small = messages.front().tool_calls.as_array()[1].as_object();
  REQUIRE_FALSE(small.at("function").as_object().at("arguments").as_object().contains("elided"));
  REQUIRE(small.at("function").as_object().at("arguments").as_object().at("command") == "ls");

  // Both receipts are still there and both ids still resolve, so the tool messages remain valid.
  AssertPairsIntact(messages);
}

TEST_CASE("An elided argument still says how much of it went", "[request]") {
  session::MessageGraph graph;
  session::AssistantPayload assistant;
  assistant.content = "writing";
  // Comfortably past `kMaxArgumentBytes`, so the tier actually fires: 500 lines of ordinary
  // text would not reach it, which is the point of a threshold measured in bytes.
  std::string body;
  for (int i = 1; i <= 500; ++i) body += "line " + std::to_string(i) + std::string(60, '.') + "\n";
  assistant.tool_calls.push_back(session::ToolCallRecord{
      "call_1", "write_file", boost::json::parse(R"({"path":"a.txt","content":""})")});
  assistant.tool_calls[0].arguments.as_object()["content"] = body;
  graph.AppendAfterLeaf(std::move(assistant));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  const auto& args = messages.front()
                         .tool_calls.as_array()[0]
                         .as_object()
                         .at("function")
                         .as_object()
                         .at("arguments")
                         .as_object();

  REQUIRE(args.at("elided") == "arguments");
  // 500 newlines mean 501 lines: the trailing newline opens a line the text never fills, and
  // the count is what the model reads, so it must be the count a text editor would show.
  REQUIRE(args.at("lines").as_uint64() == 501);
  REQUIRE(args.at("bytes").as_uint64() > 16384);
  REQUIRE(args.at("path") == "a.txt");
}

TEST_CASE("A call whose arguments are already a stand-in is not elided twice", "[request]") {
  // The counters a reader needs are the ones from the first elision. Re-measuring a stand-in
  // would report the size of the stand-in and shrink it a second time, which loses the only
  // account of how much the call really carried.
  session::MessageGraph graph;
  session::AssistantPayload assistant;
  assistant.content = "writing";
  assistant.tool_calls.push_back(session::ToolCallRecord{
      "call_1", "write_file",
      boost::json::parse(
          R"({"elided":"arguments","bytes":40000,"lines":900,"omitted":"content"})")});
  graph.AppendAfterLeaf(std::move(assistant));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  const auto& args = messages.front()
                         .tool_calls.as_array()[0]
                         .as_object()
                         .at("function")
                         .as_object()
                         .at("arguments")
                         .as_object();

  // `as_int64` here rather than `as_uint64`: these numbers come from a parsed literal, which
  // Boost stores as `int64_t`, whereas the counters the tier writes itself are `uint64_t`.
  REQUIRE(args.at("bytes").as_int64() == 40000);
  REQUIRE(args.at("lines").as_int64() == 900);
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
  core::Verdict verdict;
  verdict.command = "seq 1 3";
  verdict.ok = true;

  core::Extent extent;
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
  // The counts survive, so the model still knows how much it is not seeing.
  REQUIRE(older.at("stored").at("printed") == 3);
  REQUIRE(older.at("lines").at("first") == 1);

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
  REQUIRE(kept.at("stored").at("printed") == 3);
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
// pairs is what a projection must never break apart.
//
// Every pair here names its own `source`, so nothing supersedes anything: these are the
// results that stay live, and a test that wants a superseded one builds it separately.
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

// A run of one command, superseded by a later run of the same one: this is what produces a T1
// receipt. `n` orders the runs of that one source, not the call ids.
void AppendSupersedingRun(session::MessageGraph& graph, const std::string& source, int n,
                          const std::string& output) {
  session::AssistantPayload assistant;
  assistant.content = "calling";
  const std::string id = "call_" + source + "_" + std::to_string(n);
  assistant.tool_calls.push_back(session::ToolCallRecord{id, "execute_bash", boost::json::parse(
                                                                               R"({"command":"seq"})")});
  graph.AppendAfterLeaf(std::move(assistant));

  session::ToolPayload receipt;
  receipt.tool_call_id = id;
  receipt.tool_name = "execute_bash";
  receipt.content = PageJson(source, "b" + std::to_string(n), output, static_cast<std::size_t>(n));
  receipt.origin = paging::Origin{source, "b" + std::to_string(n), static_cast<std::size_t>(n)};
  graph.AppendAfterLeaf(std::move(receipt));
}

std::string Wide(int lines) {
  std::string text;
  for (int i = 1; i <= lines; ++i) text += "line " + std::to_string(i) + " of the output\n";
  return text;
}

std::vector<std::size_t> ToolPositions(const std::vector<ChatMessage>& messages) {
  std::vector<std::size_t> at;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (messages[i].role == "tool") at.push_back(i);
  }
  return at;
}

}  // namespace

TEST_CASE("A request with no supersession is the transcript itself", "[request]") {
  session::MessageGraph graph;
  // Six pairs is the point below which T3 does nothing, so what this case sees is the transcript
  // whole: an exploration with no repeat is not a history that needs shrinking, and no amount of
  // bulk inside one result makes the projection reach for a pair.
  for (int i = 1; i <= 6; ++i) AppendCallAndReceipt(graph, i, Wide(40));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  // Every node, and no system message because this call gave no prompt or environment.
  REQUIRE(messages.size() == graph.Size());
  for (const ChatMessage& msg : messages) {
    REQUIRE(msg.content.find("tombstone") == std::string::npos);
  }
}

TEST_CASE("Only the newest six superseded results keep a cursor", "[request]") {
  // One command, run twelve times, so eleven receipts of the twelve are superseded. Two tiers
  // then act, in order. T3 retires whole call-and-receipt pairs, oldest first, and keeps ten of
  // them -- four more than the cursor rule, so that T2 still has room to work. What is left is
  // the newest nine runs: nine superseded receipts and the live one.
  //
  // T2 then keeps a cursor on the newest six of those nine, which leaves three older superseded
  // receipts to tombstone. Both tiers remain visible, which is what the case is about: the
  // newest run is whole, the newest six superseded receipts still page, and the oldest three
  // carry a verdict with no way to ask for more.
  session::MessageGraph graph;
  for (int i = 1; i <= 12; ++i) AppendSupersedingRun(graph, "build", i, Wide(40));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  const std::vector<std::size_t> tool_at = ToolPositions(messages);
  REQUIRE(tool_at.size() == 10);

  int superseded = 0;
  int tombstones = 0;
  for (std::size_t i : tool_at) {
    const std::string& content = messages[i].content;
    const bool tombstone = content.find("\"elided\":\"tombstone\"") != std::string::npos;
    const bool superseded_receipt = content.find("\"superseded_by\"") != std::string::npos;
    if (tombstone) ++tombstones;
    if (superseded_receipt) ++superseded;
    // Never both: a receipt is in exactly one tier.
    REQUIRE_FALSE((tombstone && superseded_receipt));
  }
  // Six superseded receipts keep a cursor, three fall to a tombstone, and the newest run is
  // neither.
  REQUIRE(superseded == 6);
  REQUIRE(tombstones == 3);

  // The newest run was never superseded and is still whole.
  REQUIRE(messages[tool_at.back()].content.find("line 40 of the output") != std::string::npos);
}

TEST_CASE("A tombstone keeps the verdict and drops the cursor", "[request]") {
  // A superseded receipt still holds a `page`, which is a standing invitation to reread it. A
  // tombstone holds what happened and no way to ask for more.
  //
  // Twelve runs leave ten pairs after T3; T2 tombstones the three oldest of them and leaves the
  // newest six superseded receipts paging.
  session::MessageGraph graph;
  for (int i = 1; i <= 12; ++i) AppendSupersedingRun(graph, "build", i, Wide(40));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  const std::vector<std::size_t> tool_at = ToolPositions(messages);
  REQUIRE(tool_at.size() == 10);

  const boost::json::object tombstone = boost::json::parse(messages[tool_at.front()].content).as_object();
  REQUIRE(tombstone.contains("elided"));
  REQUIRE(boost::json::value_to<std::string>(tombstone.at("elided")) == "tombstone");
  // No cursor, and no page scope to page into.
  REQUIRE_FALSE(tombstone.contains("page"));
  REQUIRE_FALSE(tombstone.contains("superseded_by"));
  // Still attributable: which run this was is a sentence about what happened.
  REQUIRE(tombstone.contains("buffer"));
  REQUIRE(tombstone.at("buffer").as_object().contains("source"));

  // The newest run is whole, so the body it holds is readable.
  const boost::json::object newest = boost::json::parse(messages[tool_at.back()].content).as_object();
  REQUIRE_FALSE(newest.contains("elided"));
  REQUIRE(boost::json::value_to<std::string>(newest.at("output"))
              .find("line 40 of the output") != std::string::npos);
}

TEST_CASE("A registry of six or fewer superseded results keeps every cursor", "[request]") {
  // The floor is the whole rule: below it, nothing is tombstoned however large the results are.
  session::MessageGraph graph;
  for (int i = 1; i <= 7; ++i) AppendSupersedingRun(graph, "build", i, Wide(400));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  for (std::size_t i : ToolPositions(messages)) {
    REQUIRE(messages[i].content.find("tombstone") == std::string::npos);
  }
}

TEST_CASE("A result that was never superseded is never tombstoned", "[request]") {
  // The whole trigger is supersession. Ten distinct commands produce ten live results, and no
  // amount of bulk makes the projection reach for one.
  session::MessageGraph graph;
  for (int i = 1; i <= 10; ++i) AppendCallAndReceipt(graph, i, Wide(400));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});
  for (std::size_t i : ToolPositions(messages)) {
    REQUIRE(messages[i].content.find("tombstone") == std::string::npos);
    REQUIRE(messages[i].content.find("superseded_by") == std::string::npos);
  }
}

namespace {

// A delegated answer as ask_dir writes one: the conclusion, and the buffer fields that say
// which question it answered and how late that run was. It is deliberately not a page -- there
// is no `page` scope and no cursor -- because an answer has no window to read.
std::string AnswerJson(const std::string& source, const std::string& buffer, const std::string& text,
                       std::size_t rank) {
  boost::json::object result{{"success", true},
                             {"blocked", false},
                             {"output", text},
                             {"error", ""},
                             {"exit_code", 0}};
  result["buffer"] =
      boost::json::object{{"id", buffer}, {"source", source}, {"rank", rank}};
  return boost::json::serialize(result);
}

void AppendAnswer(session::MessageGraph& graph, const std::string& source, const std::string& buffer,
                  const std::string& text, std::size_t rank) {
  // The call that asked for the answer, so the receipt is not an orphan: a `tool` message whose
  // `tool_call_id` names no call is rejected by every provider.
  session::AssistantPayload assistant;
  assistant.content = "asking";
  const std::string id = "ask_" + source + "_" + std::to_string(rank);
  assistant.tool_calls.push_back(session::ToolCallRecord{id, "ask_dir",
                                                         boost::json::parse(R"({"ask":"other"})")});
  graph.AppendAfterLeaf(std::move(assistant));

  session::ToolPayload receipt;
  receipt.tool_call_id = id;
  receipt.tool_name = "ask_dir";
  receipt.content = AnswerJson(source, buffer, text, rank);
  receipt.origin = paging::Origin{source, buffer, rank};
  graph.AppendAfterLeaf(std::move(receipt));
}

}  // namespace

TEST_CASE("A second answer to one question replaces the first", "[request]") {
  // The bug this holds shut: an answer used to go in as bare prose, so no origin could be read
  // out of it and the projection had nothing to elide. Every answer to every question stayed in
  // the request at full length for as long as the conversation lasted.
  session::MessageGraph graph;
  AppendAnswer(graph, "s1", "a1", "the first answer, long enough to matter\n", 1);
  AppendAnswer(graph, "s1", "a2", "the second answer\n", 2);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  const std::vector<std::size_t> tool_at = ToolPositions(messages);
  REQUIRE(tool_at.size() == 2);

  const boost::json::value older = boost::json::parse(messages[tool_at[0]].content);
  REQUIRE(boost::json::value_to<std::string>(older.at("output")).empty());
  REQUIRE(boost::json::value_to<std::string>(older.at("superseded_by")) == "a2");

  // The newer answer is the one the model reads in full.
  REQUIRE(messages[tool_at[1]].content.find("the second answer") != std::string::npos);
}

TEST_CASE("Answers to two different questions do not replace each other", "[request]") {
  session::MessageGraph graph;
  AppendAnswer(graph, "s1", "a1", "answer about pricing\n", 1);
  AppendAnswer(graph, "s2", "a2", "answer about latency\n", 2);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  const std::vector<std::size_t> tool_at = ToolPositions(messages);
  REQUIRE(tool_at.size() == 2);
  REQUIRE(messages[tool_at[0]].content.find("answer about pricing") != std::string::npos);
  REQUIRE(messages[tool_at[1]].content.find("answer about latency") != std::string::npos);
}

TEST_CASE("A superseded page is compressed but a conclusion never is", "[request]") {
  // Two ways a result stops being re-sent, and only one of them applies to an answer. A page is
  // a window onto output that is still on disk, so dropping its body costs a cursor. An answer
  // is not: there is no cursor into another agent's run, so a tombstoned answer could only be
  // re-asked, at the price of the whole delegated turn. Supersession spends on the pages.
  session::MessageGraph graph;
  for (int i = 1; i <= 12; ++i) AppendSupersedingRun(graph, "build", i, Wide(80));
  AppendAnswer(graph, "s9", "a9", "the conclusion this turn is about: " + Wide(10), 13);

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  // Every call still has the receipt that answers it, and every receipt still has its call. T3
  // does remove pairs, so the transcript is shorter than the graph, but it never removes half of
  // one.
  REQUIRE(messages.size() < graph.Size());
  AssertPairsIntact(messages);

  int tombstones = 0;
  for (const ChatMessage& msg : messages) {
    if (msg.role == "tool" && msg.content.find("\"elided\":\"tombstone\"") != std::string::npos) {
      ++tombstones;
    }
    // Nothing that is an answer was compressed: it still holds the text it answered with.
    if (msg.role == "tool" && msg.content.find("the conclusion this turn is about") != std::string::npos) {
      REQUIRE(msg.content.find("tombstone") == std::string::npos);
    }
  }
  // The pages that fell past the cursor floor are tombstoned; the answer is never one of them.
  REQUIRE(tombstones > 0);
}

TEST_CASE("The oldest tool-call pairs are retired and no pair is ever split", "[request]") {
  // T3 is the only tier that takes messages out of the transcript, and the smallest thing it may
  // take is a pair: the assistant's `tool_calls` and the receipts that answer them. A `tool`
  // message whose `tool_call_id` names no call is rejected by every provider, and a call with no
  // receipt leaves the model waiting for a result that will never arrive. So this case checks
  // both directions, and it checks that the bound is a count rather than a size.
  session::MessageGraph graph;
  for (int i = 1; i <= 20; ++i) AppendSupersedingRun(graph, "build", i, Wide(4));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  // The graph holds twenty pairs; the projection keeps ten of them, so two hundred and twenty
  // lines of re-sent output become a hundred and ten.
  REQUIRE(graph.Size() == 40);
  REQUIRE(messages.size() == 20);

  AssertPairsIntact(messages);

  // It is the *oldest* that went: the newest run is still there, and the first run is not.
  REQUIRE(messages.back().content.find("line 4 of the output") != std::string::npos);
  for (const ChatMessage& msg : messages) {
    if (msg.role != "tool") continue;
    REQUIRE_FALSE(msg.tool_call_id == "call_build_1");
  }
}
