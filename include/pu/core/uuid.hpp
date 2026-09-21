// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <random>
#include <string>

namespace pu::uuid {

// RFC 4122 version 4, lowercase, hyphenated (8-4-4-4-12).
inline std::string Generate() {
  static thread_local std::mt19937 generator(std::random_device{}());
  static thread_local std::uniform_int_distribution<int> nibble(0, 15);
  constexpr char kHex[] = "0123456789abcdef";

  std::string id(36, '-');
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    id[i] = kHex[nibble(generator)];
  }
  id[14] = '4';
  id[19] = kHex[(nibble(generator) & 0x3) | 0x8];
  return id;
}

}  // namespace pu::uuid
