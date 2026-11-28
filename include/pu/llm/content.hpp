// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// What one message says: text, and the files or pictures it carries.
//
// A conversation is not a string. A turn may be a paragraph, a screenshot and a log file at once,
// and each provider spells those three differently on the wire -- OpenAI a `image_url` block,
// Anthropic an `image` block with a base64 source, Ollama an `images` array beside a string.
// This header is the one vocabulary those three projections read, so the difference between them
// is a translation rather than three parallel message types.
//
// **The bytes are not here.** `data` is empty for anything that came through the store, and
// `source` names the pager buffer that holds them. A message is serialised into `session.json`
// and a four-megabyte picture in that file would be a second problem with its own bounds; the
// pager already has eviction, a byte budget and a cursor that survives the buffer being paged
// away. The one exception is a block built from a caller's bytes and not yet stored, where `data`
// is what the caller handed over.
//
// A block that is not text names its own media type, because the wire needs it: `image/png` and
// `text/plain` are different blocks to every provider that has both, and a reader that guesses
// from the bytes is a reader that guesses wrong on a `.dat` that happens to start with `{`.
//

#include <string>
#include <string_view>
#include <vector>

namespace pu {

enum class BlockKind {
  kText,
  kImage,
  // A file the model is given as a document rather than as an inline picture. Not every gateway
  // has a block for one; a projection that does not says so rather than sending it as text.
  kFile,
};

inline constexpr const char* kTextBlockName = "text";
inline constexpr const char* kImageBlockName = "image";
inline constexpr const char* kFileBlockName = "file";

const char* BlockKindName(BlockKind kind);
// The kind a stored name asserts, or `kText` for a name this build does not know. A file written
// by a newer build is refused at the schema version, so an unknown name here is a name this
// version wrote and then lost track of; reading it as text is the answer that loses the least.
BlockKind BlockKindFromName(std::string_view name);

struct ContentBlock {
  BlockKind kind = BlockKind::kText;
  std::string text;        // kText: the words
  std::string data;        // base64 payload, empty when `source` names a buffer instead
  std::string media_type;  // kImage/kFile: "image/png", "text/plain"
  std::string name;        // kFile: the file's own name, for a reader that shows one
  std::string source;      // the pager buffer holding the bytes; empty for inline text

  bool IsText() const { return kind == BlockKind::kText; }
  // Whether the payload is in hand rather than named. A projection needs the bytes to build a
  // data URL, and a block that names a buffer it can no longer read is the one case it must
  // report rather than send something wrong.
  bool HasPayload() const { return !data.empty(); }
};

// A message that says one thing, which is what almost every message does. Callers that write
// text should not have to name a vector to do it.
inline std::vector<ContentBlock> TextContent(std::string text) {
  return {ContentBlock{BlockKind::kText, std::move(text), {}, {}, {}, {}}};
}

// The words of a block list, in order, with the attachments left out. This is what a reader that
// only knows text gets -- a log line, a log file's tail, a comparison -- and it is stated once
// here rather than in each of them.
std::string JoinedText(const std::vector<ContentBlock>& blocks);

// Whether a block list carries anything a reader must be shown as more than text.
bool HasAttachment(const std::vector<ContentBlock>& blocks);

}  // namespace pu
