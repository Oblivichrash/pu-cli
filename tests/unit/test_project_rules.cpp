// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kSourceDir{PU_SOURCE_DIR};

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::string> ScriptSources(const std::string& html) {
  static const std::regex kScript{R"X(<script[^>]*\ssrc="([^"]+)")X"};
  std::vector<std::string> sources;
  for (std::sregex_iterator it(html.begin(), html.end(), kScript), end; it != end; ++it) {
    sources.push_back((*it)[1].str());
  }
  return sources;
}

}  // namespace

TEST_CASE("the page loads every module it ships and ships every module it loads",
          "[rules][page]") {
  const fs::path web = kSourceDir / "web";
  const std::vector<std::string> declared = ScriptSources(ReadFile(web / "index.html"));

  std::set<std::string> loaded;
  for (const std::string& source : declared) {
    if (source.find("://") != std::string::npos) continue;
    loaded.insert(source);
  }

  std::vector<std::string> shipped;
  for (const auto& entry : fs::directory_iterator(web)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".js") continue;
    shipped.push_back(entry.path().filename().string());
  }

  std::vector<std::string> missing_tag;
  std::vector<std::string> missing_file;
  for (const std::string& name : shipped) {
    if (loaded.count(name) == 0) missing_tag.push_back(name);
  }
  for (const std::string& name : loaded) {
    if (!fs::exists(web / name)) missing_file.push_back(name);
  }

  INFO("shipped but not loaded: " << [&] {
    std::string joined;
    for (const std::string& name : missing_tag) joined += name + " ";
    return joined;
  }());
  INFO("loaded but not shipped: " << [&] {
    std::string joined;
    for (const std::string& name : missing_file) joined += name + " ";
    return joined;
  }());
  CHECK(missing_tag.empty());
  CHECK(missing_file.empty());

  fs::path app;
  for (const std::string& name : declared) {
    if (name == "app.js") app = web / name;
  }
  REQUIRE(fs::exists(app));
  const std::size_t position = [&] {
    const std::vector<std::string> order = declared;
    return static_cast<std::size_t>(
        std::find(order.rbegin(), order.rend(), std::string("app.js")) - order.rbegin());
  }();
  CHECK(position == 0);
}

TEST_CASE("every document is named by the index or by a document that links to it",
          "[rules][docs]") {
  // README promises this, and the promise went unkept: an orphan was found only when someone
  // went looking, and a note that nothing points at is a note nobody updates. The rule is one
  // line of plumbing, so the sentence in the README can be true instead of aspirational.
  const fs::path docs = kSourceDir / "docs";

  std::vector<fs::path> documents;
  for (const auto& entry : fs::recursive_directory_iterator(docs)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
    documents.push_back(entry.path());
  }
  REQUIRE_FALSE(documents.empty());

  // Everything that could name a document: the three files at the root of the repository, and
  // every document under docs/.
  std::vector<std::pair<fs::path, std::string>> corpus;
  for (const char* name : {"README.md", "CONTRIBUTING.md", "CHANGELOG.md"}) {
    const fs::path path = kSourceDir / name;
    if (fs::exists(path)) corpus.emplace_back(path, ReadFile(path));
  }
  for (const fs::path& path : documents) corpus.emplace_back(path, ReadFile(path));

  std::vector<std::string> orphans;
  for (const fs::path& path : documents) {
    const std::string name = path.filename().string();
    const bool named = std::any_of(corpus.begin(), corpus.end(), [&](const auto& entry) {
      return entry.first != path && entry.second.find(name) != std::string::npos;
    });
    if (!named) orphans.push_back(path.lexically_relative(kSourceDir).string());
  }

  INFO("orphans: " << [&] {
    std::string joined;
    for (const std::string& name : orphans) joined += name + " ";
    return joined;
  }());
  CHECK(orphans.empty());
}
