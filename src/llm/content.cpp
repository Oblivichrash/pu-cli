// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/content.hpp"

namespace pu {

const char* BlockKindName(BlockKind kind) {
  switch (kind) {
    case BlockKind::kText:
      return kTextBlockName;
    case BlockKind::kImage:
      return kImageBlockName;
    case BlockKind::kFile:
      return kFileBlockName;
  }
  return kTextBlockName;
}

BlockKind BlockKindFromName(std::string_view name) {
  if (name == kImageBlockName) return BlockKind::kImage;
  if (name == kFileBlockName) return BlockKind::kFile;
  return BlockKind::kText;
}

std::string JoinedText(const std::vector<ContentBlock>& blocks) {
  std::string out;
  for (const ContentBlock& block : blocks) {
    if (!block.IsText()) continue;
    if (!out.empty()) out.push_back('\n');
    out += block.text;
  }
  return out;
}

bool HasAttachment(const std::vector<ContentBlock>& blocks) {
  for (const ContentBlock& block : blocks) {
    if (!block.IsText()) return true;
  }
  return false;
}

}  // namespace pu
