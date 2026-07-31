// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/fact_extractor.hpp"

#include <algorithm>
#include <regex>

namespace pu {

std::vector<Artifact> FactExtractor::Extract(const std::shared_ptr<Workspace>& ctx,
                                const std::string& goal) {
  std::vector<Artifact> facts;
  if (!ctx) return facts;
  (void)goal;

  auto history = ctx->Recent(20);
  for (const auto& msg : history) {
    const std::string& text = msg.content;
    static std::regex file_re(R"((/[^\s]+\.\w+)|([a-zA-Z0-9_\-\.]+\.[a-zA-Z0-9]+))");
    std::smatch match;
    if (std::regex_search(text, match, file_re)) {
      Artifact a;
      a.type = Artifact::Type::kFilePath;
      a.content = match.str();
      a.source = msg.role;
      facts.push_back(a);
    }
    if (text.find("error") != std::string::npos ||
        text.find("fail") != std::string::npos) {
      Artifact a;
      a.type = Artifact::Type::kErrorMsg;
      a.content = text.substr(0, 200);
      a.source = msg.role;
      facts.push_back(a);
    }
  }

  std::sort(facts.begin(), facts.end(),
            [](const Artifact& a, const Artifact& b) { return a.content < b.content; });
  facts.erase(std::unique(facts.begin(), facts.end(),
                          [](const Artifact& a, const Artifact& b) {
                            return a.content == b.content;
                          }), facts.end());
  return facts;
}

}  // namespace pu
