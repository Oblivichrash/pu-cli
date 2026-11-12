// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/session/session.hpp"

namespace pu {

class Runtime;

// What routing one line produced: the text to show, whether the line was a command at all, and
// whether that command succeeded. Three facts rather than the old `bool` + `std::string&` pair,
// which could only carry two and made the caller test the string to learn the third.
//
// `routed` and `ok` are separate because the answers differ: a line that is not a command is
// not a failure, it is a line the provider should answer, while a command that failed has
// already answered and its text is the reason.
struct CommandReply {
  std::string text;
  bool routed = false;
  bool ok = true;
};

class CommandRouter {
 public:
  explicit CommandRouter(Runtime& runtime);

  CommandReply Route(const std::string& input, Session& session);

  std::string Help();

 private:
  using Args = std::span<const std::string>;

  // A handler is handed the span and the session and answers with the whole reply. It is not
  // given somewhere to write, so it cannot write twice, and it cannot answer "routed" having
  // said nothing -- the two shapes that made the previous `bool` + `std::string&` pair
  // ambiguous at the call site.
  struct Command {
    std::string help;
    std::function<CommandReply(Args, Session&)> run;
  };

  static const std::vector<std::string>& CommandOrder();

  // Three ways to answer: with text that succeeded, with text that failed, and with none. The
  // third exists because `/exit` has nothing to say, and saying that as `Replied("")` would
  // leave a reader unable to tell it from a command whose output happened to be empty.
  static CommandReply Replied(std::string text);
  static CommandReply Failed(std::string text);
  // A line that is not a command: the provider answers it, so none of this applies.
  static CommandReply NotACommand();

  CommandReply RequireMinArgs(Args args, size_t min, const std::string& usage) const;
  std::string FormatUsage(const std::string& cmd, const std::string& usage) const;

  CommandReply HandleBackend(Args args);
  CommandReply HandleModels(Args args);
  CommandReply ReportModels(const config::Backend& cfg);
  CommandReply HandleAgents(Args args, Session& session);
  CommandReply HandleRewind(Args args, Session& session);
  CommandReply HandleThinking(Args args);

  Runtime& runtime_;
  std::unordered_map<std::string, Command> commands_;
};

}  // namespace pu