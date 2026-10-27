// SPDX-License-Identifier: GPL-3.0-only
#include "pu/paging/paging.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pu::paging {

namespace {

// A trailing '\n' ends the last line rather than starting an empty one, which is what a
// terminal does with `printf 'a\n'`.
std::vector<std::size_t> IndexLines(std::string_view text) {
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == '\n') starts.push_back(i + 1);
  }
  if (text.empty()) starts.clear();
  return starts;
}

// A question's id, computed from the question rather than counted. A counter restarts
// with the process, and a resumed transcript would then read the new `s1` as the old
// `s1` — two different questions wearing one name, which is exactly the case elision
// cannot survive. FNV-1a over the key, eight hex digits: a collision needs both a
// billion-to-one roll and two questions in one transcript.
std::string SourceIdFor(std::string_view key) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char c : key) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string id = "s";
  for (int shift = 28; shift >= 0; shift -= 4) id.push_back(kHex[(hash >> shift) & 0xf]);
  return id;
}

// Absent cursors are written as null rather than left out, so a reader can tell "no page
// that way" from "a field this version does not send".
boost::json::value CursorOrNull(const std::optional<Cursor>& cursor) {
  return cursor ? boost::json::value(cursor->ToString()) : boost::json::value{};
}

// The ordinal in a buffer id, which is what says how late a run was. An id that does not
// end in digits names no order, and then no page supersedes another.
std::size_t RankOf(std::string_view buffer_id) {
  std::size_t i = 0;
  while (i < buffer_id.size() && (buffer_id[i] < '0' || buffer_id[i] > '9')) ++i;
  if (i == buffer_id.size()) return 0;
  std::size_t rank = 0;
  for (; i < buffer_id.size(); ++i) {
    if (buffer_id[i] < '0' || buffer_id[i] > '9') return 0;
    rank = rank * 10 + static_cast<std::size_t>(buffer_id[i] - '0');
  }
  return rank;
}

}  // namespace

Buffer::Buffer(std::string id, std::string source, exec::Verdict verdict, std::string text,
               exec::Extent extent)
    : id_(std::move(id)),
      source_(std::move(source)),
      verdict_(std::move(verdict)),
      text_(std::move(text)),
      extent_(extent),
      line_starts_(IndexLines(text_)) {}

std::string_view Buffer::line(std::size_t index) const {
  const std::size_t begin = line_starts_[index];
  const std::size_t end = index + 1 < line_starts_.size() ? line_starts_[index + 1] : text_.size();
  std::string_view line(text_.data() + begin, end - begin);
  if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
  return line;
}

std::string Cursor::ToString() const { return buffer + ":" + std::to_string(line); }

std::optional<Cursor> Cursor::Parse(std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string_view digits = text.substr(colon + 1);
  if (digits.size() > 12) return std::nullopt;

  std::size_t line = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return std::nullopt;
    line = line * 10 + static_cast<std::size_t>(c - '0');
  }
  return Cursor{std::string(text.substr(0, colon)), line};
}

std::optional<Anchor> Anchor::Parse(std::string_view text) {
  if (text == "head") return Head();
  if (text == "tail") return Tail();
  if (auto cursor = Cursor::Parse(text)) return At(std::move(*cursor));
  return std::nullopt;
}

Page Read(const Buffer& buffer, const Anchor& anchor, std::size_t lines) {
  Page page;
  const std::size_t retained = buffer.lines();
  if (retained == 0 || lines == 0) return page;

  std::size_t start = 0;
  if (anchor.is_cursor()) {
    start = anchor.cursor().line;
  } else if (anchor.from_end() && retained > lines) {
    start = retained - lines;
  }
  if (start >= retained) return page;

  const std::size_t end = std::min(retained, start + lines);
  // A page names lines of the command, not of the buffer, so a page of a kept end still
  // says where it sits in the output that was too large to keep.
  const std::size_t skipped = buffer.extent().skipped();
  page.first = start + 1 + skipped;
  page.last = end + skipped;

  page.text.reserve((end - start) * 64);
  for (std::size_t i = start; i < end; ++i) {
    page.text.append(buffer.line(i));
    page.text.push_back('\n');
  }

  if (start > 0) page.earlier = Cursor{buffer.id(), start > lines ? start - lines : 0};
  if (end < retained) page.later = Cursor{buffer.id(), end};
  return page;
}

boost::json::object Envelope(const Buffer& buffer, const Page& page) {
  const exec::Extent& extent = buffer.extent();

  // Three scopes, named: where this page sits, which run produced it, how much of the run
  // survived. A single object holding all three is what makes a result hard to read.
  return boost::json::object{
      {"page",
       boost::json::object{
           {"first", page.first},
           {"last", page.last},
           {"earlier", CursorOrNull(page.earlier)},
           {"later", CursorOrNull(page.later)},
       }},
      {"buffer", boost::json::object{{"id", buffer.id()}, {"source", buffer.source()}}},
      {"extent",
       boost::json::object{
           {"lines", extent.lines},
           {"retained", extent.retained},
           {"truncated", extent.truncated()},
       }},
  };
}

std::optional<Origin> OriginOf(std::string_view receipt) {
  if (receipt.empty() || receipt.front() != '{') return std::nullopt;
  try {
    const boost::json::value parsed = boost::json::parse(receipt);
    if (!parsed.is_object()) return std::nullopt;
    const boost::json::object& result = parsed.as_object();
    const auto held = result.find("buffer");
    if (held == result.end() || !held->value().is_object()) return std::nullopt;
    const boost::json::object& fields = held->value().as_object();

    const auto source = fields.find("source");
    const auto buffer = fields.find("id");
    if (source == fields.end() || buffer == fields.end()) return std::nullopt;
    if (!source->value().is_string() || !buffer->value().is_string()) return std::nullopt;

    Origin origin{std::string(source->value().as_string()),
                  std::string(buffer->value().as_string()), 0};
    if (origin.source.empty() || origin.buffer.empty()) return std::nullopt;
    origin.rank = RankOf(origin.buffer);
    return origin;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string SupersededReceipt(std::string_view receipt, std::string_view superseded_by) {
  boost::json::value parsed = boost::json::parse(receipt);
  boost::json::object& result = parsed.as_object();
  result["output"] = "";
  result["superseded_by"] = superseded_by;
  return boost::json::serialize(parsed);
}

Pager::Pager(Budget budget) : budget_(budget) {}

Budget Pager::budget() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return budget_;
}

void Pager::SetBudget(Budget budget) {
  std::lock_guard<std::mutex> lock(mutex_);
  budget_ = budget;
}

std::shared_ptr<const Buffer> Pager::Store(exec::Captured captured, exec::Verdict verdict,
                                           std::string source_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = "o" + std::to_string(next_id_++);
  const std::string source = source_key.empty() ? std::string{} : SourceIdFor(source_key);

  const std::size_t bytes = captured.text.size();
  auto buffer = std::make_shared<const Buffer>(id, std::move(source), std::move(verdict),
                                               std::move(captured.text), captured.extent);

  buffers_.push_back(buffer);
  retained_bytes_ += bytes;
  // The page just stored outlives the budget that would evict it: a command that printed
  // one huge line is still readable once.
  while (buffers_.size() > std::max<std::size_t>(1, budget_.retain_buffers) ||
         (retained_bytes_ > budget_.retain_bytes && buffers_.size() > 1)) {
    retained_bytes_ -= buffers_.front()->bytes();
    buffers_.erase(buffers_.begin());
  }
  return buffer;
}

std::shared_ptr<const Buffer> Pager::Find(std::string_view id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& buffer : buffers_) {
    if (buffer->id() == id) return buffer;
  }
  return nullptr;
}

}  // namespace pu::paging
