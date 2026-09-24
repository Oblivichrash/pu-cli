// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Text produced outside pu-cli (subprocess output, file contents) arrives in
// whatever encoding its producer used, while everything pu-cli stores, serialises
// or sends to a provider must be valid UTF-8.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pu::text {

inline constexpr std::string_view kUtf8ReplacementCharacter = "\xEF\xBF\xBD";  // U+FFFD

namespace detail {

inline constexpr unsigned int kMaxCodePoint = 0x10FFFF;
inline constexpr unsigned int kSurrogateFirst = 0xD800;
inline constexpr unsigned int kSurrogateLast = 0xDFFF;

constexpr bool IsContinuationByte(unsigned char byte) { return (byte & 0xC0) == 0x80; }

constexpr std::size_t SequenceLength(unsigned char lead) {
  struct LeadPattern {
    unsigned char mask;
    unsigned char value;
    std::size_t length;
  };
  constexpr LeadPattern kPatterns[] = {
      {0x80, 0x00, 1},
      {0xE0, 0xC0, 2},
      {0xF0, 0xE0, 3},
      {0xF8, 0xF0, 4},
  };
  for (const LeadPattern& pattern : kPatterns) {
    if ((lead & pattern.mask) == pattern.value) return pattern.length;
  }
  return 0;
}

constexpr unsigned int DecodeCodePoint(std::string_view text, std::size_t index,
                                       std::size_t length) {
  constexpr unsigned char kLeadPayloadMask[] = {0x7F, 0x1F, 0x0F, 0x07};
  constexpr unsigned char kContinuationPayloadMask = 0x3F;

  unsigned int code_point = static_cast<unsigned char>(text[index]) & kLeadPayloadMask[length - 1];
  for (std::size_t offset = 1; offset < length; ++offset) {
    code_point = (code_point << 6) |
                 (static_cast<unsigned char>(text[index + offset]) & kContinuationPayloadMask);
  }
  return code_point;
}

constexpr bool IsOverlong(std::size_t length, unsigned int code_point) {
  return (length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
         (length == 4 && code_point < 0x10000);
}

constexpr bool IsSurrogate(unsigned int code_point) {
  return code_point >= kSurrogateFirst && code_point <= kSurrogateLast;
}

// Length of the well-formed sequence starting at `text[index]`, or nothing when
// the bytes there are not one.
inline std::optional<std::size_t> ValidSequenceLength(std::string_view text, std::size_t index) {
  const std::size_t length = SequenceLength(static_cast<unsigned char>(text[index]));
  if (length == 0 || index + length > text.size()) return std::nullopt;

  for (std::size_t offset = 1; offset < length; ++offset) {
    if (!IsContinuationByte(static_cast<unsigned char>(text[index + offset]))) {
      return std::nullopt;
    }
  }

  const unsigned int code_point = DecodeCodePoint(text, index, length);
  if (IsOverlong(length, code_point) || IsSurrogate(code_point) || code_point > kMaxCodePoint) {
    return std::nullopt;
  }
  return length;
}

}  // namespace detail

inline bool IsValidUtf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const std::optional<std::size_t> length = detail::ValidSequenceLength(text, index);
    if (!length) return false;
    index += *length;
  }
  return true;
}

inline std::string SanitizeUtf8(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const std::optional<std::size_t> length = detail::ValidSequenceLength(text, index);
    if (!length) {
      result += kUtf8ReplacementCharacter;
      ++index;
      continue;
    }
    result.append(text.data() + index, *length);
    index += *length;
  }
  return result;
}

}  // namespace pu::text
