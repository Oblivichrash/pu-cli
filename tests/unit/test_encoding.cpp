// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/platform.hpp"
#include "pu/exec/capture.hpp"
#include "pu/core/text.hpp"
#include "pu/mcp/stdio_transport.hpp"
#include "pu/tools/tool.hpp"

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

std::string CaptureFirstLine(const std::string& command, const std::vector<std::string>& args) {
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
  REQUIRE(text::IsValidUtf8("\xE4\xB8\xAD"));      // U+4E2D, 3 bytes
  REQUIRE(text::IsValidUtf8("\xF0\x9F\x98\x80"));  // U+1F600, 4 bytes
  REQUIRE(text::IsValidUtf8("mixed \xE4\xB8\xAD ascii"));
}

TEST_CASE("IsValidUtf8 rejects malformed sequences", "[text]") {
  REQUIRE_FALSE(text::IsValidUtf8("\xB2\xBB"));          // GBK "not", illegal lead
  REQUIRE_FALSE(text::IsValidUtf8("\xE4\xB8"));          // truncated 3-byte
  REQUIRE_FALSE(text::IsValidUtf8("\xC0\x80"));          // overlong NUL
  REQUIRE_FALSE(text::IsValidUtf8("\xED\xA0\x80"));      // surrogate U+D800
  REQUIRE_FALSE(text::IsValidUtf8("\xF5\x80\x80\x80"));  // above U+10FFFF
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

TEST_CASE("Tool results parse back when the output is not UTF-8", "[tools][text]") {
  const std::string gbk_stdout = "\xB2\xBB\xCA\xC7\xC4\xDA\xB2\xBF\xBB\xF2";

  const std::string result =
      tools::MakeToolResultJson(false, gbk_stdout, "Command failed (exit 1)", 1);

  const auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("exit_code") == 1);
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("output"))));
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("output"))));

  const auto parsed = tools::ParseToolResult(result);
  REQUIRE(parsed.valid);
  REQUIRE_FALSE(parsed.success);
  REQUIRE(parsed.exit_code == 1);
}

TEST_CASE("Tool results keep valid text byte for byte", "[tools][text]") {
  const std::string utf8_stdout = "success \xE4\xB8\xAD\xE6\x96\x87";
  const std::string result = tools::MakeToolResultJson(true, utf8_stdout, "", 0);

  const auto j = boost::json::parse(result);
  REQUIRE(boost::json::value_to<std::string>(j.at("output")) == utf8_stdout);
}

TEST_CASE("Run returns valid UTF-8 for localized shell output", "[exec][text]") {
  const std::string output =
      pu::exec::Run("nonexistent_command_pu_encoding_test", pu::CancelToken{}, "", 0).captured.text;

  REQUIRE_FALSE(output.empty());
  REQUIRE(text::IsValidUtf8(output));
}

TEST_CASE("Process output decoding repairs non-UTF-8 pipe text", "[platform][text]") {
  const std::string piped = "\xD6\xD0\xCE\xC4";
  REQUIRE_FALSE(text::IsValidUtf8(piped));

  const std::string decoded = pu::platform::FromPipedOutput(piped);
  REQUIRE(text::IsValidUtf8(decoded));
#ifdef _WIN32
  if (GetACP() == 936) REQUIRE(decoded == "\xE4\xB8\xAD\xE6\x96\x87");
#endif
}

TEST_CASE("Process output decoding leaves UTF-8 untouched", "[platform][text]") {
  const std::string utf8 = "\xE4\xB8\xAD\xE6\x96\x87";

  // Each decode is a pass-through only when its own code page is UTF-8. Under an ANSI code
  // page the same bytes are valid in both encodings, so the decoder reads them as that code
  // page and must not be expected to leave them alone. The two functions key off different
  // code pages: the piped one off the system ANSI page, the console one off the console's.
  bool piped_pass_through = true;
  bool console_pass_through = true;
#ifdef _WIN32
  piped_pass_through = (GetACP() == CP_UTF8);
  console_pass_through = (GetConsoleOutputCP() == CP_UTF8);
#endif

  if (piped_pass_through) REQUIRE(pu::platform::FromPipedOutput(utf8) == utf8);
  if (console_pass_through) REQUIRE(pu::platform::FromConsoleOutput(utf8) == utf8);

  // Whatever the code page, the result is text the rest of the program can carry.
  REQUIRE(text::IsValidUtf8(pu::platform::FromPipedOutput(utf8)));
  REQUIRE(text::IsValidUtf8(pu::platform::FromConsoleOutput(utf8)));
}

TEST_CASE("Process output decoding always yields valid UTF-8", "[platform][text]") {
  const std::string undecodable = "ok \xFF\xFE tail";
  REQUIRE(text::IsValidUtf8(pu::platform::FromPipedOutput(undecodable)));
  REQUIRE(text::IsValidUtf8(pu::platform::FromConsoleOutput(undecodable)));
  REQUIRE(pu::platform::FromPipedOutput("") == "");
}

TEST_CASE("MCP stdio transport delivers code-page bytes as UTF-8", "[mcp][text]") {
  const FileEmitter emitter("\xD6\xD0\xCE\xC4\n");

  const std::string received = CaptureFirstLine(emitter.command(), emitter.args());

  REQUIRE_FALSE(received.empty());
  REQUIRE(text::IsValidUtf8(received));
#ifdef _WIN32
  if (GetACP() == 936) REQUIRE(received == "\xE4\xB8\xAD\xE6\x96\x87");
#endif
}

TEST_CASE("A transport error is read back as its reason", "[text][errors]") {
  // What boost::system hands back: the reason, then where it was raised. The second half is
  // the implementation's, and a caller deciding what to do next gains nothing from it.
  const std::string raised =
      "connect: Connection refused [system:111 at /usr/include/boost/asio/detail/"
      "reactive_socket_service.hpp:590:5 in function 'boost::system::error_code "
      "boost::asio::detail::reactive_socket_service<Protocol>::connect(implementation_type&, "
      "const endpoint_type&, boost::system::error_code&)']";
  REQUIRE(pu::text::ReadableError(raised) == "connect: Connection refused");

  // Nothing to cut is left alone, and an unclosed bracket is not guessed at.
  REQUIRE(pu::text::ReadableError("Request cancelled") == "Request cancelled");
  REQUIRE(pu::text::ReadableError("HTTP error 401: bad key [oops") ==
          "HTTP error 401: bad key [oops");
}
