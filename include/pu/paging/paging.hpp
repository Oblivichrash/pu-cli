// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pu/exec/capture.hpp"

namespace pu::paging {

// What a session will spend on keeping output: lines per page, bytes per command, and
// how many commands stay readable. One struct, one owner — the pager — so a tool cannot
// be asked for a page size the store it writes to does not agree with.
struct Budget {
  std::size_t page_lines = 120;
  std::size_t max_lines = 400;
  std::size_t capture_bytes = 262144;
  std::size_t retain_bytes = 4u << 20;
  std::size_t retain_buffers = 8;
};

// A place in one command's output: the buffer, and the 0-based line within it.
struct Cursor {
  std::string buffer;
  std::size_t line = 0;

  std::string ToString() const;  // <buffer>:<line>
  static std::optional<Cursor> Parse(std::string_view text);
};

// Where a read starts. `head` and `tail` name a first look, and therefore a fresh run;
// `at` names a page of a run already stored. One value in place of a view flag, a
// cursor, and a rule about which of the two wins.
class Anchor {
 public:
  static Anchor Head() { return Anchor{Kind::kHead, {}}; }
  static Anchor Tail() { return Anchor{Kind::kTail, {}}; }
  static Anchor At(Cursor cursor) { return Anchor{Kind::kCursor, std::move(cursor)}; }
  static std::optional<Anchor> Parse(std::string_view text);

  bool from_end() const { return kind_ == Kind::kTail; }
  bool is_cursor() const { return kind_ == Kind::kCursor; }
  const Cursor& cursor() const { return cursor_; }

 private:
  enum class Kind { kHead, kTail, kCursor };
  Anchor(Kind kind, Cursor cursor) : kind_(kind), cursor_(std::move(cursor)) {}

  Kind kind_ = Kind::kHead;
  Cursor cursor_;
};

// One command's output: kept whole, indexed by line, immutable once stored.
class Buffer {
 public:
  Buffer(std::string id, std::string source, exec::Verdict verdict, std::string text,
         exec::Extent extent);

  const std::string& id() const { return id_; }
  const std::string& source() const { return source_; }
  const exec::Verdict& verdict() const { return verdict_; }
  const exec::Extent& extent() const { return extent_; }

  std::size_t lines() const { return line_starts_.size(); }
  std::size_t bytes() const { return text_.size(); }
  std::string_view line(std::size_t index) const;

 private:
  std::string id_;
  std::string source_;
  exec::Verdict verdict_;
  std::string text_;
  exec::Extent extent_;
  std::vector<std::size_t> line_starts_;
};

// A contiguous run of a buffer's lines, numbered against the command that printed them
// and carrying the cursor that reaches either neighbour.
struct Page {
  std::string text;
  std::size_t first = 0;  // 1-based against the command's whole output; 0 when empty
  std::size_t last = 0;
  std::optional<Cursor> earlier;
  std::optional<Cursor> later;
};

Page Read(const Buffer& buffer, const Anchor& anchor, std::size_t lines);

// The buffers a session can still read. A page in flight holds its buffer through the
// shared_ptr, so eviction never pulls the text out from under it.
class Pager {
 public:
  explicit Pager(Budget budget = {});

  // A copy, because the store may be re-budgeted between two calls; the caller keeps
  // the value it was given rather than a reference into the mutex.
  Budget budget() const;
  void SetBudget(Budget budget);

  // A run's output and its verdict, stored under the question it answered. Two runs of
  // one question share a source id and keep separate buffer ids.
  std::shared_ptr<const Buffer> Store(exec::Captured captured, exec::Verdict verdict,
                                      std::string source_key = {});
  std::shared_ptr<const Buffer> Find(std::string_view id) const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<const Buffer>> buffers_;  // oldest first
  std::map<std::string, std::string> sources_;          // question -> its source id
  std::size_t retained_bytes_ = 0;
  Budget budget_;
  std::size_t next_id_ = 1;
  std::size_t next_source_id_ = 1;
};

}  // namespace pu::paging
