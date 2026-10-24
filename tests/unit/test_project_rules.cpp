// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
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
