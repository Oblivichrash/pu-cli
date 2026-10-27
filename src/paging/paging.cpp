// SPDX-License-Identifier: GPL-3.0-only
#include "pu/paging/paging.hpp"

#include <algorithm>
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

  // A source is minted once and keeps its id, so a later run of the same question is
  // recognisably the same question.
  std::string source;
  if (!source_key.empty()) {
    auto [it, inserted] = sources_.try_emplace(std::move(source_key), std::string{});
    if (inserted) it->second = "s" + std::to_string(next_source_id_++);
    source = it->second;
  }

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
