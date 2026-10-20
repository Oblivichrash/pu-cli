// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

// The rules in CONTRIBUTING.md that a machine can decide are decided here instead of by a
// reviewer. Each case names the rule it enforces, and each one reads the tree the build was
// configured from, so a new file that breaks a rule fails the suite rather than a review.
namespace {

namespace fs = std::filesystem;

const fs::path kSourceDir{PU_SOURCE_DIR};

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  for (std::string line; std::getline(file, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

std::vector<fs::path> FilesWithExtension(const char* root, const std::set<std::string>& extensions) {
  std::vector<fs::path> found;
  const fs::path base = kSourceDir / root;
  if (!fs::exists(base)) return found;
  for (const auto& entry : fs::recursive_directory_iterator(base)) {
    if (!entry.is_regular_file()) continue;
    if (extensions.count(entry.path().extension().string()) == 0) continue;
    found.push_back(entry.path());
  }
  std::sort(found.begin(), found.end());
  return found;
}

fs::path Relative(const fs::path& path) {
  return fs::relative(path, kSourceDir);
}

// A heading's anchor as GitHub renders it: lowercased, punctuation dropped, spaces to dashes.
// The approximation is deliberate — it catches a link that names a heading that is not there,
// which is the failure that happens, and it does not try to resolve collisions.
std::string Anchor(const std::string& heading) {
  std::string text;
  bool in_backticks = false;
  bool in_link_text = false;
  for (std::size_t i = 0; i < heading.size(); ++i) {
    const char c = heading[i];
    if (c == '`') {
      in_backticks = !in_backticks;
      continue;
    }
    if (c == '[') {
      in_link_text = true;
      continue;
    }
    if (c == ']') {
      in_link_text = false;
      continue;
    }
    if (in_link_text && c == '(') {
      while (i < heading.size() && heading[i] != ')') ++i;
      continue;
    }
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == ' ' || c == '-' || c == '_';
    if (!keep) continue;
    text.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  std::string slug;
  for (const char c : text) slug.push_back(c == ' ' ? '-' : c);
  return slug;
}

std::set<std::string> AnchorsOf(const fs::path& markdown) {
  std::set<std::string> anchors;
  for (const std::string& line : ReadLines(markdown)) {
    if (line.empty() || line[0] != '#') continue;
    const std::size_t hashes = line.find_first_not_of('#');
    if (hashes == std::string::npos || hashes > 6 || hashes >= line.size()) continue;
    if (line[hashes] != ' ') continue;
    anchors.insert(Anchor(line.substr(hashes + 1)));
  }
  return anchors;
}

struct MarkdownLink {
  fs::path file;
  std::string target;
};

std::vector<MarkdownLink> LinksIn(const fs::path& markdown) {
  static const std::regex kLink{R"X(\[[^\]]*\]\(([^)]+)\))X"};
  std::vector<MarkdownLink> links;
  const std::string text = ReadFile(markdown);
  for (std::sregex_iterator it(text.begin(), text.end(), kLink), end; it != end; ++it) {
    links.push_back({markdown, (*it)[1].str()});
  }
  return links;
}

std::vector<fs::path> AllMarkdown() { return FilesWithExtension(".", {".md"}); }

bool IsExternal(const std::string& target) {
  return target.empty() || target.find("://") != std::string::npos ||
         target.rfind("mailto:", 0) == 0;
}

fs::path ResolveTarget(const fs::path& from, const std::string& target) {
  const std::string path = target.substr(0, target.find('#'));
  return path.empty() ? from : fs::weakly_canonical(from.parent_path() / path);
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

TEST_CASE("every source file states its licence on the first line", "[rules][licence]") {
  std::vector<std::string> missing;
  for (const fs::path& file : FilesWithExtension("src", {".cpp", ".hpp"})) {
    if (ReadLines(file).front().find("SPDX-License-Identifier") == std::string::npos) {
      missing.push_back(Relative(file).string());
    }
  }
  for (const fs::path& file : FilesWithExtension("include", {".cpp", ".hpp"})) {
    if (ReadLines(file).front().find("SPDX-License-Identifier") == std::string::npos) {
      missing.push_back(Relative(file).string());
    }
  }
  for (const fs::path& file : FilesWithExtension("tests", {".cpp", ".hpp"})) {
    if (ReadLines(file).front().find("SPDX-License-Identifier") == std::string::npos) {
      missing.push_back(Relative(file).string());
    }
  }
  INFO("missing an SPDX line: " << [&] {
    std::string joined;
    for (const std::string& name : missing) joined += name + " ";
    return joined;
  }());
  CHECK(missing.empty());
}

TEST_CASE("every test file is named by the build", "[rules][tests]") {
  // The source list, not the whole file: a name that only appears in a comment does not
  // compile the file, so it does not count as naming it.
  std::set<std::string> listed;
  for (const std::string& line : ReadLines(kSourceDir / "tests" / "CMakeLists.txt")) {
    const std::size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    const std::string trimmed = line.substr(start);
    if (trimmed.rfind("unit/", 0) == 0) listed.insert(trimmed);
  }

  std::vector<std::string> unregistered;
  for (const fs::path& file : FilesWithExtension("tests/unit", {".cpp"})) {
    if (listed.count("unit/" + file.filename().string()) == 0) {
      unregistered.push_back(file.filename().string());
    }
  }
  INFO("not in tests/CMakeLists.txt: " << [&] {
    std::string joined;
    for (const std::string& name : unregistered) joined += name + " ";
    return joined;
  }());
  CHECK(unregistered.empty());
}

TEST_CASE("every document is reachable from a document outside docs", "[rules][docs]") {
  const std::vector<fs::path> markdown = AllMarkdown();
  std::set<fs::path> seen;
  std::vector<fs::path> frontier;
  for (const fs::path& file : markdown) {
    if (Relative(file).begin()->string() != "docs") frontier.push_back(file);
  }

  std::vector<fs::path> reachable;
  while (!frontier.empty()) {
    const fs::path file = frontier.back();
    frontier.pop_back();
    if (!seen.insert(fs::weakly_canonical(file)).second) continue;
    reachable.push_back(file);
    for (const MarkdownLink& link : LinksIn(file)) {
      if (IsExternal(link.target)) continue;
      const fs::path target = ResolveTarget(file, link.target);
      if (fs::exists(target) && target.extension() == ".md") frontier.push_back(target);
    }
  }

  std::vector<std::string> orphans;
  for (const fs::path& file : markdown) {
    if (seen.count(fs::weakly_canonical(file)) != 0) continue;
    if (std::find(reachable.begin(), reachable.end(), file) != reachable.end()) continue;
    orphans.push_back(Relative(file).string());
  }
  INFO("nothing links here: " << [&] {
    std::string joined;
    for (const std::string& name : orphans) joined += name + " ";
    return joined;
  }());
  CHECK(orphans.empty());
}

TEST_CASE("every relative link and every anchor names something that is there", "[rules][docs]") {
  std::vector<std::string> broken;
  for (const fs::path& file : AllMarkdown()) {
    for (const MarkdownLink& link : LinksIn(file)) {
      if (IsExternal(link.target)) continue;
      const fs::path target = ResolveTarget(file, link.target);
      if (!fs::exists(target)) {
        broken.push_back(Relative(file).string() + " -> " + link.target);
        continue;
      }
      const std::size_t hash = link.target.find('#');
      if (hash == std::string::npos || target.extension() != ".md") continue;
      const std::string fragment = link.target.substr(hash + 1);
      if (fragment.empty()) continue;
      if (AnchorsOf(target).count(fragment) == 0) {
        broken.push_back(Relative(file).string() + " -> " + link.target);
      }
    }
  }
  INFO("unresolved: " << [&] {
    std::string joined;
    for (const std::string& name : broken) joined += name + " ";
    return joined;
  }());
  CHECK(broken.empty());
}

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

TEST_CASE("a CHANGELOG section names each kind of change once", "[rules][changelog]") {
  const std::vector<std::string> lines = ReadLines(kSourceDir / "CHANGELOG.md");
  std::vector<std::string> duplicates;
  std::string section;
  std::set<std::string> headings;
  for (const std::string& line : lines) {
    if (line.rfind("## ", 0) == 0) {
      section = line;
      headings.clear();
      continue;
    }
    if (line.rfind("### ", 0) != 0) continue;
    if (!headings.insert(line).second) duplicates.push_back(section + " / " + line);
  }
  INFO("repeated: " << [&] {
    std::string joined;
    for (const std::string& name : duplicates) joined += name + " ";
    return joined;
  }());
  CHECK(duplicates.empty());
}
