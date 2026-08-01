// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/artifact_extractor.hpp"

#include <algorithm>
#include <regex>

namespace pu {

std::vector<Artifact> ArtifactExtractor::Extract(const std::shared_ptr<Workspace>& ctx,
                                const std::string& goal) {
  std::vector<Artifact> artifacts;
  if (!ctx) return artifacts;
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
      artifacts.push_back(a);
    }
    if (text.find("error") != std::string::npos ||
        text.find("fail") != std::string::npos) {
      Artifact a;
      a.type = Artifact::Type::kErrorMsg;
      a.content = text.substr(0, 200);
      a.source = msg.role;
      artifacts.push_back(a);
    }
  }

  std::sort(artifacts.begin(), artifacts.end(),
            [](const Artifact& a, const Artifact& b) { return a.content < b.content; });
  artifacts.erase(std::unique(artifacts.begin(), artifacts.end(),
                          [](const Artifact& a, const Artifact& b) {
                            return a.content == b.content;
                          }), artifacts.end());
  return artifacts;
}

}  // namespace pu
