// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::core {

struct Fact {
  enum class Type {
    kFilePath,
    kErrorMsg,
    kFunctionName,
    kUserPreference,
    kCodeSnippet,
    kUrl,
    kCommand,
    kOther
  };

  Type type = Type::kOther;
  std::string content;
  std::string source;
  double confidence = 1.0;

  Fact() = default;
  Fact(Type t, std::string c, std::string src = "", double conf = 1.0)
      : type(t), content(std::move(c)), source(std::move(src)), confidence(conf) {}
};

using FactList = std::vector<Fact>;

}  // namespace pu::core
