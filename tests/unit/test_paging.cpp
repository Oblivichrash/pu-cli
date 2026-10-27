// SPDX-License-Identifier: GPL-3.0-only
#include "pu/paging/paging.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pu;
using namespace pu::paging;

namespace {

std::size_t CountLines(const std::string& text) {
  if (text.empty()) return 0;
  std::size_t lines = 0;
  for (const char c : text) {
    if (c == '\n') ++lines;
  }
  return text.back() == '\n' ? lines : lines + 1;
}

exec::Extent ExtentOf(const std::string& text, std::optional<std::size_t> printed = std::nullopt,
                      bool from_end = false) {
  exec::Extent extent;
  extent.bytes = text.size();
  extent.retained = CountLines(text);
  extent.lines = printed.value_or(extent.retained);
  extent.from_end = from_end;
  return extent;
}

exec::Verdict VerdictOf(const std::string& command) {
  exec::Verdict verdict;
  verdict.command = command;
  verdict.ok = true;
  return verdict;
}

std::shared_ptr<const Buffer> MakeBuffer(const std::string& text,
                                         std::optional<std::size_t> printed = std::nullopt,
                                         bool from_end = false) {
  return std::make_shared<const Buffer>("o1", "", VerdictOf("seq 1 10"), text,
                                        ExtentOf(text, printed, from_end));
}

std::string Numbered(std::size_t count) {
  std::string text;
  for (std::size_t i = 1; i <= count; ++i) text += std::to_string(i) + "\n";
  return text;
}

}  // namespace

TEST_CASE("A buffer's lines end where its newlines do", "[paging]") {
  auto buffer = MakeBuffer("alpha\nbeta\n");
  REQUIRE(buffer->lines() == 2);
  REQUIRE(buffer->line(0) == "alpha");
  REQUIRE(buffer->line(1) == "beta");

  auto unterminated = MakeBuffer("alpha\nbeta");
  REQUIRE(unterminated->lines() == 2);
  REQUIRE(unterminated->line(1) == "beta");

  REQUIRE(MakeBuffer("")->lines() == 0);
}

TEST_CASE("A blank line is still a line", "[paging]") {
  auto buffer = MakeBuffer("a\n\nb\n");
  REQUIRE(buffer->lines() == 3);
  REQUIRE(buffer->line(1).empty());
  REQUIRE(buffer->line(2) == "b");
}

TEST_CASE("A first look starts at the head or at the end", "[paging]") {
  auto buffer = MakeBuffer(Numbered(10));

  const auto head = Read(*buffer, Anchor::Head(), 3);
  REQUIRE(head.text == "1\n2\n3\n");
  REQUIRE(head.first == 1);
  REQUIRE(head.last == 3);

  const auto tail = Read(*buffer, Anchor::Tail(), 3);
  REQUIRE(tail.text == "8\n9\n10\n");
  REQUIRE(tail.first == 8);
  REQUIRE(tail.last == 10);

  // Fewer lines than a page is the whole output, from either end.
  REQUIRE(Read(*buffer, Anchor::Head(), 20).first == 1);
  REQUIRE(Read(*buffer, Anchor::Tail(), 20).first == 1);
}

TEST_CASE("A page names the pages either side of it", "[paging]") {
  auto buffer = MakeBuffer(Numbered(10));

  const auto head = Read(*buffer, Anchor::Head(), 3);
  REQUIRE_FALSE(head.earlier.has_value());
  REQUIRE(head.later.has_value());
  REQUIRE(head.later->ToString() == "o1:3");

  const auto middle = Read(*buffer, Anchor::At(Cursor{"o1", 3}), 3);
  REQUIRE(middle.text == "4\n5\n6\n");
  REQUIRE(middle.earlier->line == 0);
  REQUIRE(middle.later->line == 6);

  const auto tail = Read(*buffer, Anchor::Tail(), 3);
  REQUIRE(tail.earlier->line == 4);
  REQUIRE_FALSE(tail.later.has_value());
}

TEST_CASE("A page past the end holds no line", "[paging]") {
  auto buffer = MakeBuffer(Numbered(3));
  const auto page = Read(*buffer, Anchor::At(Cursor{"o1", 3}), 5);
  REQUIRE(page.first == 0);
  REQUIRE(page.last == 0);
  REQUIRE(page.text.empty());
}

TEST_CASE("A page of a truncated head counts from the first line", "[paging]") {
  auto buffer = MakeBuffer(Numbered(2), 500);
  const auto page = Read(*buffer, Anchor::Head(), 10);

  REQUIRE(buffer->extent().truncated());
  REQUIRE(buffer->extent().skipped() == 0);
  REQUIRE(page.first == 1);
  REQUIRE(page.last == 2);
  REQUIRE_FALSE(page.later.has_value());
}

TEST_CASE("A page of a truncated end counts against the whole output", "[paging]") {
  // The last two lines of five hundred, which is what an end-kept capture leaves.
  auto buffer = MakeBuffer("499\n500\n", 500, true);
  const auto page = Read(*buffer, Anchor::Tail(), 10);

  REQUIRE(buffer->extent().skipped() == 498);
  REQUIRE(page.first == 499);
  REQUIRE(page.last == 500);
}

TEST_CASE("A cursor names the buffer and the line", "[paging]") {
  REQUIRE(Cursor{"o3", 120}.ToString() == "o3:120");

  const auto parsed = Cursor::Parse("o3:120");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->buffer == "o3");
  REQUIRE(parsed->line == 120);
}

TEST_CASE("A string that is not a cursor is refused", "[paging]") {
  REQUIRE_FALSE(Cursor::Parse("").has_value());
  REQUIRE_FALSE(Cursor::Parse("o1").has_value());
  REQUIRE_FALSE(Cursor::Parse(":3").has_value());
  REQUIRE_FALSE(Cursor::Parse("o1:").has_value());
  REQUIRE_FALSE(Cursor::Parse("o1:1a").has_value());
  REQUIRE_FALSE(Cursor::Parse("o1:-1").has_value());
  REQUIRE_FALSE(Cursor::Parse("o1:9999999999999999").has_value());
}

TEST_CASE("An anchor is one value with three forms", "[paging]") {
  REQUIRE(Anchor::Parse("head")->is_cursor() == false);
  REQUIRE(Anchor::Parse("head")->from_end() == false);
  REQUIRE(Anchor::Parse("tail")->from_end() == true);

  const auto at = Anchor::Parse("o3:120");
  REQUIRE(at.has_value());
  REQUIRE(at->is_cursor());
  REQUIRE(at->cursor().line == 120);

  REQUIRE_FALSE(Anchor::Parse("middle").has_value());
  REQUIRE_FALSE(Anchor::Parse("").has_value());
}

TEST_CASE("A buffer is found by the id its cursor names", "[paging]") {
  Pager pager;
  const auto stored = pager.Store(exec::Captured{"a\nb\n", ExtentOf("a\nb\n")}, VerdictOf("ls"));
  REQUIRE(stored->id() == "o1");

  const auto found = pager.Find("o1");
  REQUIRE(found != nullptr);
  REQUIRE(found->lines() == 2);
  REQUIRE(pager.Find("o404") == nullptr);
}

TEST_CASE("The pager drops the oldest buffer when the budget is spent", "[paging]") {
  Budget budget;
  budget.retain_bytes = 16;
  Pager pager(budget);

  auto first = pager.Store(exec::Captured{std::string(10, 'a'), {}}, VerdictOf("ls"));
  auto second = pager.Store(exec::Captured{std::string(10, 'b'), {}}, VerdictOf("ls"));

  REQUIRE(pager.Find(first->id()) == nullptr);
  REQUIRE(pager.Find(second->id()) != nullptr);
  // The buffer just stored survives even when it alone exceeds the budget.
  REQUIRE(second->lines() == 1);
}

TEST_CASE("The pager keeps the number of buffers its budget names", "[paging]") {
  Pager pager;
  std::vector<std::string> ids;
  for (std::size_t i = 0; i < pager.budget().retain_buffers + 2; ++i) {
    ids.push_back(pager.Store(exec::Captured{"x" + std::to_string(i) + "\n", {}},
                              VerdictOf("ls"))
                      ->id());
  }

  REQUIRE(pager.Find(ids.front()) == nullptr);
  REQUIRE(pager.Find(ids.back()) != nullptr);
}

TEST_CASE("Two runs of one command share a source and not a buffer", "[paging]") {
  Pager pager;
  auto first = pager.Store(exec::Captured{"build 1\n", {}}, VerdictOf("ninja -j8"),
                           "ninja -j8\x1f/repo");
  auto second = pager.Store(exec::Captured{"build 2\n", {}}, VerdictOf("ninja -j8"),
                            "ninja -j8\x1f/repo");
  auto elsewhere = pager.Store(exec::Captured{"other\n", {}}, VerdictOf("ninja -j8"),
                               "ninja -j8\x1f/tmp");

  REQUIRE(first->source() == second->source());
  REQUIRE(first->id() != second->id());
  REQUIRE(elsewhere->source() != first->source());
}

TEST_CASE("A run stored without a source has no source id", "[paging]") {
  Pager pager;
  REQUIRE(pager.Store(exec::Captured{"a\n", {}}, VerdictOf("ls"))->source().empty());
}
