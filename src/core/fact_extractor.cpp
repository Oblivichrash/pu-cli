// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/fact_extractor.hpp"

#include <algorithm>
#include <regex>

namespace pu::core {

FactList FactExtractor::Extract(const std::shared_ptr<Context>& ctx,
                                const std::string& goal) {
  FactList facts;
  if (!ctx) return facts;
  (void)goal;

  auto history = ctx->Recent(20);
  for (const auto& msg : history) {
    const std::string& text = msg.content;
    static std::regex file_re(R"((/[^\s]+\.\w+)|([a-zA-Z0-9_\-\.]+\.[a-zA-Z0-9]+))");
    std::smatch match;
    if (std::regex_search(text, match, file_re)) {
      facts.emplace_back(Fact::Type::kFilePath, match.str(), msg.role);
    }
    if (text.find("error") != std::string::npos ||
        text.find("fail") != std::string::npos) {
      facts.emplace_back(Fact::Type::kErrorMsg, text.substr(0, 200), msg.role);
    }
  }

  std::sort(facts.begin(), facts.end(),
            [](const Fact& a, const Fact& b) { return a.content < b.content; });
  facts.erase(std::unique(facts.begin(), facts.end(),
                          [](const Fact& a, const Fact& b) {
                            return a.content == b.content;
                          }), facts.end());
  return facts;
}

}  // namespace pu::core
