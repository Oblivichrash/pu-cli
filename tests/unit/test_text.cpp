// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/mcp/stdio_transport.hpp"
#include "pu/tools/tool_result.hpp"

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace pu;

namespace {

namespace fs = std::filesystem;

// A child that copies a file to stdout verbatim, so the bytes reaching the
// transport are exactly the bytes written here.
class FileEmitter {
 public:
  explicit FileEmitter(const std::string& bytes) {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
            ("pu_encoding_" + std::to_string(counter.fetch_add(1)) + ".bin");
    std::ofstream out(path_, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
#ifdef _WIN32
    command_ = "cmd";
    args_ = {"/c", "type", path_.string()};
#else
    command_ = "cat";
    args_ = {path_.string()};
#endif
  }

  ~FileEmitter() {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  FileEmitter(const FileEmitter&) = delete;
  FileEmitter& operator=(const FileEmitter&) = delete;

  const std::string& command() const { return command_; }
  const std::vector<std::string>& args() const { return args_; }

 private:
  fs::path path_;
  std::string command_;
  std::vector<std::string> args_;
};

std::string CaptureFirstLine(const std::string& command,
                             const std::vector<std::string>& args) {
  mcp::StdioTransport transport(command, args);
  std::mutex mutex;
  std::condition_variable ready;
  std::string received;
  transport.Start([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    if (received.empty()) received = line;
    ready.notify_one();
  });

  std::unique_lock<std::mutex> lock(mutex);
  ready.wait_for(lock, std::chrono::seconds(10), [&] { return !received.empty(); });
  lock.unlock();
  transport.Stop();
  return received;
}

}  // namespace

TEST_CASE("IsValidUtf8 accepts ASCII and well-formed sequences", "[text]") {
  REQUIRE(text::IsValidUtf8(""));
  REQUIRE(text::IsValidUtf8("plain ascii"));
  REQUIRE(text::IsValidUtf8("\xE4\xB8\xAD"));           // U+4E2D, 3 bytes
  REQUIRE(text::IsValidUtf8("\xF0\x9F\x98\x80"));       // U+1F600, 4 bytes
  REQUIRE(text::IsValidUtf8("mixed \xE4\xB8\xAD ascii"));
}

TEST_CASE("IsValidUtf8 rejects malformed sequences", "[text]") {
  REQUIRE_FALSE(text::IsValidUtf8("\xB2\xBB"));         // GBK "not", illegal lead
  REQUIRE_FALSE(text::IsValidUtf8("\xE4\xB8"));         // truncated 3-byte
  REQUIRE_FALSE(text::IsValidUtf8("\xC0\x80"));         // overlong NUL
  REQUIRE_FALSE(text::IsValidUtf8("\xED\xA0\x80"));     // surrogate U+D800
  REQUIRE_FALSE(text::IsValidUtf8("\xF5\x80\x80\x80")); // above U+10FFFF
  REQUIRE_FALSE(text::IsValidUtf8("\xFF"));
  REQUIRE_FALSE(text::IsValidUtf8("ok \xB2\xBB ok"));
}

TEST_CASE("SanitizeUtf8 repairs malformed input and keeps valid text", "[text]") {
  const std::string gbk_text = "error: \xB2\xBB\xCA\xC7 command";
  REQUIRE_FALSE(text::IsValidUtf8(gbk_text));

  const std::string repaired = text::SanitizeUtf8(gbk_text);
  REQUIRE(text::IsValidUtf8(repaired));
  REQUIRE(repaired.find("error: ") == 0);
  REQUIRE(repaired.find(" command") != std::string::npos);
  REQUIRE(repaired.find(text::kUtf8ReplacementCharacter) != std::string::npos);
}

TEST_CASE("SanitizeUtf8 leaves valid input untouched", "[text]") {
  const std::string valid = "ascii \xE4\xB8\xAD\xE6\x96\x87 tail";
  REQUIRE(text::SanitizeUtf8(valid) == valid);
  REQUIRE(text::SanitizeUtf8("") == "");
}

TEST_CASE("Tool results parse back when the output is not UTF-8",
          "[tools][text]") {
  // GBK bytes for a localized "not found" message.
  const std::string gbk_stdout = "\xB2\xBB\xCA\xC7\xC4\xDA\xB2\xBF\xBB\xF2";

  const std::string result = tools::MakeToolResultJson(
      false, gbk_stdout, gbk_stdout, "Command failed (exit 1)", 1);

  const auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("exit_code") == 1);
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("stdout"))));
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("stderr"))));

  const auto parsed = tools::ParseToolResult(result);
  REQUIRE(parsed.valid);
  REQUIRE_FALSE(parsed.success);
  REQUIRE(parsed.exit_code == 1);
}

TEST_CASE("Tool results keep valid text byte for byte", "[tools][text]") {
  const std::string utf8_stdout = "success \xE4\xB8\xAD\xE6\x96\x87";
  const std::string result =
      tools::MakeToolResultJson(true, utf8_stdout, "", "", 0);

  const auto j = boost::json::parse(result);
  REQUIRE(boost::json::value_to<std::string>(j.at("stdout")) == utf8_stdout);
}

TEST_CASE("ExecuteCommand returns valid UTF-8 for localized shell output",
          "[platform][text]") {
  // A localized shell message is not UTF-8 until the capture path decodes it.
  std::string output;
  pu::platform::ExecuteCommand("nonexistent_command_pu_encoding_test", output);

  REQUIRE_FALSE(output.empty());
  REQUIRE(text::IsValidUtf8(output));
}

TEST_CASE("Process output decoding repairs non-UTF-8 pipe text",
          "[platform][text]") {
  // Bytes Python writes to a pipe on a Chinese Windows host (cp936): invalid as
  // UTF-8, so a JSON-RPC line carrying them could not be parsed.
  const std::string piped = "\xD6\xD0\xCE\xC4";
  REQUIRE_FALSE(text::IsValidUtf8(piped));

  const std::string decoded = pu::platform::FromPipedOutput(piped);
  REQUIRE(text::IsValidUtf8(decoded));
#ifdef _WIN32
  // The pipe path decodes with the ANSI code page; a console code page here
  // would produce valid but wrong text.
  if (GetACP() == 936) REQUIRE(decoded == "\xE4\xB8\xAD\xE6\x96\x87");
#endif
}

TEST_CASE("Process output decoding leaves UTF-8 untouched", "[platform][text]") {
  const std::string utf8 = "\xE4\xB8\xAD\xE6\x96\x87";
  REQUIRE(pu::platform::FromPipedOutput(utf8) == utf8);
  REQUIRE(pu::platform::FromConsoleOutput(utf8) == utf8);
}

TEST_CASE("Process output decoding always yields valid UTF-8",
          "[platform][text]") {
  // Bytes that no code page maps cleanly still have to produce parseable text.
  const std::string undecodable = "ok \xFF\xFE tail";
  REQUIRE(text::IsValidUtf8(pu::platform::FromPipedOutput(undecodable)));
  REQUIRE(text::IsValidUtf8(pu::platform::FromConsoleOutput(undecodable)));
  REQUIRE(pu::platform::FromPipedOutput("") == "");
}

TEST_CASE("MCP stdio transport delivers code-page bytes as UTF-8",
          "[mcp][text]") {
  // The child emits its bytes verbatim, so these arrive exactly as a JSON-RPC
  // response line would: cp936 for two CJK characters, which is not valid UTF-8.
  const FileEmitter emitter("\xD6\xD0\xCE\xC4\n");

  const std::string received = CaptureFirstLine(emitter.command(), emitter.args());

  REQUIRE_FALSE(received.empty());
  REQUIRE(text::IsValidUtf8(received));
#ifdef _WIN32
  if (GetACP() == 936) REQUIRE(received == "\xE4\xB8\xAD\xE6\x96\x87");
#endif
}
