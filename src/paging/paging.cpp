// SPDX-License-Identifier: GPL-3.0-only
#include "pu/paging/paging.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <random>
#include <utility>

#include "pu/core/json.hpp"

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

// The mark of one pager, four hex digits. An id is read back by someone who did not write it —
// a resumed transcript, another process's answer — and a bare `o1` would then name whatever this
// stored first rather than the run the cursor meant. With the mark, a cursor from elsewhere
// finds nothing, which is an answer the tool can defend; without it, it finds a stranger's
// output and cannot tell.
std::string NewGeneration() {
  static std::mt19937 engine(std::random_device{}());
  static std::mutex guard;
  std::lock_guard<std::mutex> lock(guard);
  const unsigned value = static_cast<unsigned>(engine()) & 0xffffu;
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (int shift = 12; shift >= 0; shift -= 4) text.push_back(kHex[(value >> shift) & 0xf]);
  return text;
}

// A page that names no order is never superseded, and one written before `rank` was a field
// of its own names none: keeping a page too long costs tokens, while calling the wrong page
// the older one loses it.
std::size_t RankIn(const boost::json::value& object) {
  if (!object.is_object()) return 0;
  const boost::json::object& fields = object.as_object();
  const auto order = fields.find("rank");
  if (order == fields.end()) return 0;
  const boost::json::value& value = order->value();
  if (value.is_uint64()) return static_cast<std::size_t>(value.as_uint64());
  if (value.is_int64() && value.as_int64() > 0) {
    return static_cast<std::size_t>(value.as_int64());
  }
  return 0;
}

}  // namespace

// A question's id, computed from the question rather than counted. A counter restarts
// with the process, and a resumed transcript would then read the new `s1` as the old
// `s1` -- two different questions wearing one name, which is exactly the case elision
// cannot survive. FNV-1a over the key, eight hex digits: a collision needs both a
// billion-to-one roll and two questions in one transcript.
//
// Public because the id is a fact about a question, not about the store: a delegated answer
// carries the same kind of origin and is never written to a pager, so its source has to be
// derivable by whoever writes it down.
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

Buffer::Buffer(std::string id, std::string source, exec::Verdict verdict, std::string text,
               exec::Extent extent, std::size_t rank)
    : id_(std::move(id)),
      source_(std::move(source)),
      verdict_(std::move(verdict)),
      text_(std::move(text)),
      extent_(extent),
      rank_(rank),
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

namespace {

// A bare number is a line of the newest run, not a cursor: the buffer a cursor names is a
// thing the model never chose and cannot reason about, and requiring it taught the model a
// vocabulary that only exists inside this process. A run names no buffer here, so `ReadAt`
// resolves `kNewestBuffer` to whatever is stored now.
std::optional<std::size_t> ParseLineNumber(std::string_view text) {
  if (text.empty() || text.size() > 12) return std::nullopt;
  std::size_t line = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    line = line * 10 + static_cast<std::size_t>(c - '0');
  }
  return line;
}

}  // namespace

std::optional<Anchor> Anchor::Parse(std::string_view text) {
  if (text == "head") return Head();
  if (text == "tail") return Tail();
  // A cursor first: it carries its own buffer, so it answers without the newest-run rule.
  if (auto cursor = Cursor::Parse(text)) return At(std::move(*cursor));
  // A bare line number is sent as 1-based, the way a line is printed by `grep -n` and by
  // the page's own `first`/`last`, and is stored 0-based like every other cursor.
  if (auto line = ParseLineNumber(text)) {
    return At(Cursor{std::string(kNewestBuffer), *line > 0 ? *line - 1 : 0});
  }
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

Matches Search(const Buffer& buffer, std::string_view needle, std::size_t context,
               std::size_t max_lines) {
  Matches result;
  const std::size_t total = buffer.lines();
  if (needle.empty() || total == 0 || max_lines == 0) return result;

  const std::size_t skipped = buffer.extent().skipped();
  std::size_t last_shown = 0;
  bool have_shown = false;

  for (std::size_t i = 0; i < total; ++i) {
    if (buffer.line(i).find(needle) == std::string_view::npos) continue;
    ++result.count;

    // Context is emitted once per match, and the lines between two matches are shared rather
    // than repeated: two hits twenty lines apart read as one block, which is what a person
    // reading `grep -C` sees and what keeps a wall of repeats out of the transcript.
    const std::size_t first = i >= context ? i - context : 0;
    const std::size_t last = std::min(total - 1, i + context);
    for (std::size_t line = first; line <= last; ++line) {
      if (have_shown && line <= last_shown) continue;
      if (result.shown >= max_lines) {
        result.truncated = true;
        return result;
      }
      // Prefixed with the command's own line number, so the model can ask for more of it by
      // name rather than by counting from the top of the page.
      result.text.append(std::to_string(line + 1 + skipped));
      result.text.push_back(':');
      result.text.append(buffer.line(line));
      result.text.push_back('\n');
      last_shown = line;
      have_shown = true;
    }
    ++result.shown;
  }
  return result;
}

boost::json::object Envelope(const Buffer& buffer, const Page& page) {
  const exec::Extent& extent = buffer.extent();

  // A result says three things a model needs and nothing about this process: which lines it
  // is looking at, how much of the command's output survived, and how to see more. The
  // readable fields are those three. The `buffer` block is the fourth, and it is not for the
  // model: `OriginOf` and `TombstonedReceipt` read it to know which run this was and whether
  // a newer one replaced it, so it cannot be dropped without silently disabling supersession.
  // It is kept because a machine needs it, and the tool description stops teaching it.
  const std::size_t printed = extent.lines;
  const std::size_t kept = extent.retained;
  const std::size_t dropped = printed > kept ? printed - kept : 0;
  return boost::json::object{
      {"lines",
       boost::json::object{
           {"first", page.first},
           {"last", page.last},
           {"of", printed},
       }},
      // Named `stored` and not `output`: a tool result already has an `output` field holding
      // the text, and the envelope's keys are merged over that result's own. The bytes are
      // one thing and the account of how many there were is another.
      {"stored",
       boost::json::object{
           {"printed", printed},
           {"kept", kept},
           {"dropped", dropped},
           // Which part of the command's output the kept lines are: the start, or the end.
           // Naming it is what turns `dropped` into a decision -- dropped from the start
           // means the text still on file is the tail, and `grep` will not find a line that
           // was never kept.
           {"kept_from", extent.from_end ? "the end" : "the start"},
       }},
      // More of this run is readable without running anything. Absent when the whole output
      // is already in hand, so a model that has everything is never told to go looking.
      {"read_more",
       dropped == 0 ? boost::json::value{}
                    : boost::json::value{boost::json::object{
                          {"how", "call execute_bash again with at=<line number>, or "
                                  "with query=<text> to find the lines containing it"},
                          {"covered", boost::json::object{{"first", 0}, {"last", kept}}},
                      }}},
      {"buffer",
       boost::json::object{
           {"id", buffer.id()},
           {"source", buffer.source()},
           {"rank", buffer.rank()},
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
    origin.rank = RankIn(held->value());
    return origin;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

boost::json::value OriginToJson(const Origin& origin) {
  return boost::json::value{
      {"source", origin.source},
      {"buffer", origin.buffer},
      {"rank", origin.rank},
  };
}

std::optional<Origin> OriginFromJson(const boost::json::value& value) {
  if (!value.is_object()) return std::nullopt;

  Origin origin;
  origin.source = json::ValueOrDefault<std::string>(value, "source", "");
  origin.buffer = json::ValueOrDefault<std::string>(value, "buffer", "");
  if (origin.source.empty() || origin.buffer.empty()) return std::nullopt;
  origin.rank = RankIn(value);
  return origin;
}

std::string SupersededReceipt(std::string_view receipt, std::string_view superseded_by) {
  boost::json::value parsed = boost::json::parse(receipt);
  boost::json::object& result = parsed.as_object();
  result["output"] = "";
  result["superseded_by"] = superseded_by;
  return boost::json::serialize(parsed);
}

std::string TombstonedReceipt(std::string_view receipt) {
  // Not an envelope: a refusal, a plain string, an empty body. There is no verdict inside to
  // lift out, and inventing one would put a success the call never had in front of the model.
  if (receipt.empty() || receipt.front() != '{') return std::string(receipt);

  boost::json::value parsed;
  try {
    parsed = boost::json::parse(receipt);
  } catch (const std::exception&) {
    return std::string(receipt);
  }
  if (!parsed.is_object()) return std::string(receipt);
  const boost::json::object& held = parsed.as_object();

  // Rebuilt field by field rather than copied and pruned: a caller's `extra` fields are not
  // known here, and an allow-list is the only way this stays a compression of the envelope
  // rather than a place for a future field to be forgotten in and re-shipped every turn.
  boost::json::object tombstone;
  const auto lift = [&held, &tombstone](std::string_view key) {
    const auto found = held.find(key);
    if (found != held.end()) tombstone[key] = found->value();
  };
  // What happened.
  lift("success");
  lift("blocked");
  lift("error");
  lift("exit_code");
  lift("refusal");
  // Which run it was, and how much of it there was. `id` and `source` are the pair that keeps
  // this result attributable across turns; the counts say how much is not being shown.
  if (const auto buffer = held.find("buffer"); buffer != held.end() && buffer->value().is_object()) {
    const boost::json::object& fields = buffer->value().as_object();
    boost::json::object kept;
    for (const std::string_view key : {"id", "source"}) {
      const auto found = fields.find(key);
      if (found != fields.end()) kept[key] = found->value();
    }
    tombstone["buffer"] = std::move(kept);
  }
  // How much came out. `stored` is the current name; a receipt written by a build that sent
  // `extent` still carries that, so both are lifted and either one survives.
  for (const std::string_view key : {"stored", "extent"}) {
    const auto found = held.find(key);
    if (found != held.end() && found->value().is_object()) tombstone[key] = found->value();
  }
  // The command itself is the thing a model repeats, so it is carried in full even though it
  // is the biggest single field here. This is the argv axis, and it is the one axis this
  // function does not compress. A paged result keeps it under `verdict`; a raw
  // `execute_bash` result keeps it at the top, so both are lifted and whichever exists wins.
  if (const auto verdict = held.find("verdict"); verdict != held.end() && verdict->value().is_object()) {
    const boost::json::object& fields = verdict->value().as_object();
    boost::json::object kept;
    for (const std::string_view key : {"command", "cwd", "exit_code", "ok", "error", "stopped"}) {
      const auto found = fields.find(key);
      if (found != fields.end()) kept[key] = found->value();
    }
    if (!kept.empty()) tombstone["verdict"] = std::move(kept);
  }
  lift("command");
  lift("cwd");

  // A conclusion is not a window. An answer that carries a `buffer` but nothing a reader can
  // page into -- no `lines`, no `stored` -- has, and the run behind it is another agent's,
  // which this conversation cannot page into. Compressing it would leave a receipt that names
  // a question and can only be re-asked, at the price of the whole delegated turn. So it is
  // kept whole: the budget buys back nothing here, and that is the cheaper mistake.
  const bool conclusion = tombstone.contains("buffer") && !tombstone.contains("lines") &&
                          !tombstone.contains("stored") && !tombstone.contains("extent");
  if (conclusion) return std::string(receipt);

  // What is gone, said plainly: a result that silently lost its body reads as a command that
  // printed nothing, and a model that believes that will run the command again.
  tombstone["elided"] = "tombstone";
  return boost::json::serialize(tombstone);
}

Pager::Pager(Budget budget) : budget_(budget), generation_(NewGeneration()) {}

Budget Pager::budget() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return budget_;
}

void Pager::SetBudget(Budget budget) {
  std::lock_guard<std::mutex> lock(mutex_);
  budget_ = budget;
}

void Pager::ContinueRankingFrom(std::size_t last_rank) {
  std::lock_guard<std::mutex> lock(mutex_);
  next_rank_ = std::max(next_rank_, last_rank + 1);
}

std::shared_ptr<const Buffer> Pager::Store(exec::Captured captured, exec::Verdict verdict,
                                           std::string source_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = "o" + generation_ + "-" + std::to_string(next_ordinal_++);
  const std::string source = source_key.empty() ? std::string{} : SourceIdFor(source_key);
  const std::size_t rank = next_rank_++;

  const std::size_t bytes = captured.text.size();
  auto buffer = std::make_shared<const Buffer>(id, std::move(source), std::move(verdict),
                                               std::move(captured.text), captured.extent, rank);

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
  // A bare line number names the newest run, and that is a rule about this store rather
  // than about the cursor, so it is resolved here where the ordering is known.
  if (id == kNewestBuffer) return buffers_.empty() ? nullptr : buffers_.back();
  for (const auto& buffer : buffers_) {
    if (buffer->id() == id) return buffer;
  }
  return nullptr;
}

std::shared_ptr<const Buffer> Pager::Newest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.empty() ? nullptr : buffers_.back();
}

std::shared_ptr<const Buffer> Pager::Record(std::string text, exec::Verdict verdict,
                                            std::string source_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = "a" + generation_ + "-" + std::to_string(next_ordinal_++);
  const std::string source = source_key.empty() ? std::string{} : SourceIdFor(source_key);
  const std::size_t rank = next_rank_++;

  // The extent is the answer's own: what it is, and all of it retained. There is no second
  // window onto it, so `retained == lines` and nothing downstream reads it as truncated.
  exec::Extent extent;
  extent.lines = 1;
  extent.bytes = text.size();
  extent.retained = 1;

  auto buffer = std::make_shared<const Buffer>(id, std::move(source), std::move(verdict),
                                               std::move(text), extent, rank);
  // Kept outside the buffer list on purpose: the list is what eviction walks, and it is sized
  // by what a *command* retains. An answer is a few hundred lines at most, and it is the text
  // the conversation is about, so nothing here may drop it while a receipt still names it.
  //
  // That is why `Find` can never return one of these, and why the projection must not treat the
  // `buffer` on an answer as a page it may elide. A body that names no cursor and no extent is
  // a conclusion, not a window: taking it away leaves a receipt that can only be re-asked, at
  // the price of the whole delegated turn again. Dropping the text is worth less than keeping
  // the answer.
  return buffer;
}

}  // namespace pu::paging
