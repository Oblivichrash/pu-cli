// SPDX-License-Identifier: GPL-3.0-only

#include "pu/conversation_store.hpp"
#include "core/error.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using namespace pu;

namespace {

std::filesystem::path MakeTempDir() {
  auto path = std::filesystem::temp_directory_path() / "pu_test_store";
  std::filesystem::create_directories(path);
  return path;
}

Conversation MakeSampleConv() {
  Conversation conv;
  conv.id = "test-conv-001";
  conv.created_at = "2026-04-28T10:00:00Z";
  conv.updated_at = "2026-04-28T10:30:00Z";

  conv.messages = {
    {1, "2026-04-28T10:00:01Z", "user", "Hello"},
    {2, "2026-04-28T10:00:02Z", "chat", "Hi there!"},
    {3, "2026-04-28T10:00:03Z", "user", "@bash list files"},
    {4, "2026-04-28T10:00:04Z", "bash", "Executed ls -l"},
  };

  conv.expert_histories["chat"] = {
    {1, "2026-04-28T10:00:01Z", "user", "Hello"},
    {2, "2026-04-28T10:00:02Z", "chat", "Hi there!"},
  };
  conv.expert_histories["bash"] = {
    {3, "2026-04-28T10:00:03Z", "user", "@bash list files"},
    {4, "2026-04-28T10:00:04Z", "bash", "Executed ls -l"},
  };

  return conv;
}

}  // namespace

TEST_CASE("ConversationStore save and load roundtrip", "[store]") {
  auto dir = MakeTempDir();
  ConversationStore store(dir);
  auto original = MakeSampleConv();

  store.Save(original);

  auto loaded = store.Load(original.id);
  REQUIRE(loaded.id == original.id);
  REQUIRE(loaded.created_at == original.created_at);
  REQUIRE(loaded.updated_at == original.updated_at);
  REQUIRE(loaded.messages.size() == original.messages.size());
  REQUIRE(loaded.expert_histories.size() == original.expert_histories.size());

  REQUIRE(loaded.messages[0].role == "user");
  REQUIRE(loaded.messages[0].content == "Hello");
  REQUIRE(loaded.messages[1].role == "chat");
  REQUIRE(loaded.messages[2].role == "user");
  REQUIRE(loaded.messages[3].role == "bash");

  REQUIRE(loaded.expert_histories["chat"].size() == 2);
  REQUIRE(loaded.expert_histories["bash"].size() == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("ConversationStore throws on non-existent id", "[store]") {
  auto dir = MakeTempDir();
  ConversationStore store(dir);

  REQUIRE_THROWS_AS(store.Load("nonexistent"), StoreError);

  std::filesystem::remove_all(dir);
}

TEST_CASE("ConversationStore throws on invalid JSON", "[store]") {
  auto dir = MakeTempDir();
  std::ofstream file(dir / "bad.json");
  file << "this is not json";
  file.close();

  ConversationStore store(dir);
  REQUIRE_THROWS_AS(store.Load("bad"), StoreError);

  std::filesystem::remove_all(dir);
}

TEST_CASE("ConversationStore list conversations", "[store]") {
  auto dir = MakeTempDir();
  ConversationStore store(dir);

  auto conv1 = MakeSampleConv();
  conv1.id = "conv1";
  auto conv2 = MakeSampleConv();
  conv2.id = "conv2";

  store.Save(conv1);
  store.Save(conv2);

  auto list = store.List();
  REQUIRE(list.size() == 2);

  bool found1 = false, found2 = false;
  for (auto& c : list) {
    if (c.id == "conv1") found1 = true;
    if (c.id == "conv2") found2 = true;
  }
  REQUIRE(found1);
  REQUIRE(found2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("ConversationStore ExportMarkdown contains messages", "[store]") {
  auto dir = MakeTempDir();
  ConversationStore store(dir);

  auto conv = MakeSampleConv();
  conv.id = "export-test";

  store.Save(conv);

  std::string md = store.ExportMarkdown("export-test");
  REQUIRE(md.find("# Conversation: export-test") != std::string::npos);
  REQUIRE(md.find("user") != std::string::npos);
  REQUIRE(md.find("chat") != std::string::npos);
  REQUIRE(md.find("bash") != std::string::npos);
  REQUIRE(md.find("Hello") != std::string::npos);

  std::filesystem::remove_all(dir);
}
