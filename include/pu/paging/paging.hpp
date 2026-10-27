// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/exec/run.hpp"

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
         exec::Extent extent, std::size_t rank = 0);

  const std::string& id() const { return id_; }
  const std::string& source() const { return source_; }
  const exec::Verdict& verdict() const { return verdict_; }
  const exec::Extent& extent() const { return extent_; }
  // The run's place in the order runs were started. Only ranks from one chain of runs compare,
  // which is why a pager opening a transcript continues its numbering instead of restarting;
  // 0 means the run names no order, and then no page supersedes any.
  std::size_t rank() const { return rank_; }

  std::size_t lines() const { return line_starts_.size(); }
  std::size_t bytes() const { return text_.size(); }
  std::string_view line(std::size_t index) const;

 private:
  std::string id_;
  std::string source_;
  exec::Verdict verdict_;
  std::string text_;
  exec::Extent extent_;
  std::size_t rank_ = 0;
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

// A page's wire form, in both directions: the three scopes a result carries beside its
// text, and the way back in — where a page came from, and a page whose run has since been
// replaced. One module writes and reads it, because a writer in one place and a reader in
// another is how the two drift apart.
boost::json::object Envelope(const Buffer& buffer, const Page& page);

// What a result says about where it came from: the question it answered, the run that
// answered it, and how late that run was. A receipt carrying all three is a page;
// nothing else is a candidate for replacement.
struct Origin {
  std::string source;
  std::string buffer;
  // The run's place in the order runs were started, which is what makes one page newer
  // than another. 0 where the id does not name one, and then no page supersedes any.
  std::size_t rank = 0;
};

std::optional<Origin> OriginOf(std::string_view receipt);

// A page of an older run: the verdict and the window stay, the text goes, and the run
// that replaced it is named so the model can ask for this one back by its cursor.
std::string SupersededReceipt(std::string_view receipt, std::string_view superseded_by);

// The buffers a session can still read. A page in flight holds its buffer through the
// shared_ptr, so eviction never pulls the text out from under it.
class Pager {
 public:
  explicit Pager(Budget budget = {});

  // A copy, because the store may be re-budgeted between two calls; the caller keeps
  // the value it was given rather than a reference into the mutex.
  Budget budget() const;
  void SetBudget(Budget budget);

  // Ranks compare against each other, so a pager opening a transcript continues the order it
  // finds there rather than starting over: `last_rank` is the highest rank in that transcript,
  // and the next stored run takes the one after it. Without this, a run made after a restart
  // would rank below every page the transcript already carries and elision would stop firing.
  void ContinueRankingFrom(std::size_t last_rank);

  // A run's output and its verdict, stored under the question it answered. Two runs of
  // one question share a source id and keep separate buffer ids; the source is derived from
  // the key rather than counted, so a resumed transcript still reads a question as itself.
  // The buffer id names the run *and this pager*, so a cursor written elsewhere — by an
  // earlier process, by a peer — finds nothing instead of finding a namesake.
  std::shared_ptr<const Buffer> Store(exec::Captured captured, exec::Verdict verdict,
                                      std::string source_key = {});
  std::shared_ptr<const Buffer> Find(std::string_view id) const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<const Buffer>> buffers_;  // oldest first
  std::size_t retained_bytes_ = 0;
  Budget budget_;
  // Four hex digits, one per pager: the part of an id that keeps two processes' first run
  // from wearing one name.
  std::string generation_;
  std::size_t next_ordinal_ = 1;
  std::size_t next_rank_ = 1;
};

}  // namespace pu::paging
