// SPDX-License-Identifier: GPL-3.0-only
//
// `pu ask` and `pu chat`: the two commands that take a turn in the foreground. They share a
// file because they are the same act — a prompt in, a turn out — and differ only in whether
// the conversation outlives the prompt: `ask` settles the session before the turn, so the next
// question inherits nothing, and `chat` keeps it, so the next line continues the first.
//
#include "pu/app/cli.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pu/core/platform.hpp"
#include "pu/runtime/answer.hpp"
#include "pu/runtime/runtime.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

namespace {

// The media type a path's own bytes name. Read from the extension rather than sniffed: the
// wire wants the type the sender asserts, and a magic-byte guess would disagree with the file
// the user named in the cases where they differ.
std::string MediaTypeOf(const std::filesystem::path& path) {
  const std::string ext = path.extension().string();
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".bmp") return "image/bmp";
  if (ext == ".txt" || ext == ".md" || ext == ".log") return "text/plain";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".json") return "application/json";
  return {};
}

// A file named on the command line, as the block the turn will carry. Nothing is stored yet --
// the runtime does that -- so this is only the read and the kind.
std::optional<Runtime::Attachment> ReadAttachment(const std::string& named) {
  const std::filesystem::path path(named);
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return std::nullopt;
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return std::nullopt;

  Runtime::Attachment attachment;
  attachment.media_type = MediaTypeOf(path);
  attachment.name = path.filename().string();
  attachment.bytes = std::move(bytes);
  // A picture is a picture and everything else is a file. The kind decides which wire block
  // carries it, and a gateway that has none for a file refuses the turn rather than guessing.
  attachment.kind = attachment.media_type.rfind("image/", 0) == 0 ? BlockKind::kImage
                                                                  : BlockKind::kFile;
  return attachment;
}

}  // namespace

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime,
           bool show_usage, const std::vector<std::string>& attachments) {
  // Read before the runtime is built: a path that cannot be opened is a mistake in the
  // invocation, and a turn sent without it would answer a different question than the one the
  // command line asked.
  std::vector<Runtime::Attachment> carried;
  carried.reserve(attachments.size());
  for (const std::string& named : attachments) {
    std::optional<Runtime::Attachment> attachment = ReadAttachment(named);
    if (!attachment) {
      std::cerr << "cannot attach '" << named << "': it is missing, unreadable or empty\n";
      return static_cast<int>(ExitCode::kUsage);
    }
    carried.push_back(std::move(*attachment));
  }
  // The prompt is the whole of what this turn knows. `chat` and `serve` remember, and are
  // where a conversation that continues belongs; an answer asked for once should not move the
  // conversation the next `pu ask` inherits.
  return WithInitializedRuntime(
      agent, runtime,
      [&] {
        ExecutionResult result =
            runtime.ProcessInput(prompt, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken(), std::move(carried));
        ::pu::runtime::PrintAnswer(result, std::cout);
        detail::PrintFailure(result);
        detail::PrintNotice(result);
        if (show_usage) detail::PrintUsage(result);
        return static_cast<int>(detail::ExitCodeForAsk(result));
      },
      /*ephemeral=*/true);
}

int RunChat(const std::string& agent, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    const config::AgentEntry& active = runtime.ActiveAgent();
    std::string agent_info = "Connected to agent: " + active.name;
    if (!active.description.empty()) agent_info += " (" + active.description + ")";
    // Said to the reader, not logged: these two lines are the banner a person answers, and a
    // banner written through the log carries a timestamp and a level it has no use for. It also
    // puts a greeting in the log file every run, where it is noise between the entries a
    // failure is looked up in. stderr because it is a remark beside the turn, like the notices,
    // and stdout stays the answer. Printed only where someone is reading: a piped `pu chat`
    // has no banner to show and nothing to gain from one appearing in the transcript.
    if (platform::StdinIsTerminal()) {
      std::cerr << agent_info << "\n";
      std::cerr << "Type /help for available commands.\n";
    }

    std::string input;
    while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
      if (input.empty()) continue;

      if (input == "/exit" || input == "/quit") break;

      try {
        ExecutionResult result =
            runtime.ProcessInput(input, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken());
        ::pu::runtime::PrintAnswer(result, std::cout);
        detail::PrintFailure(result);
        detail::PrintNotice(result);
      } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
      }
    }
    std::cout << "\nGoodbye!\n";
    return static_cast<int>(ExitCode::kOk);
  });
}
}  // namespace pu::cli
