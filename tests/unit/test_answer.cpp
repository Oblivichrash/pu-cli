// SPDX-License-Identifier: GPL-3.0-only

//
// What a finished turn says, and which stream it says it on.
//
// This used to be three copies of the same `if` — one in `ask`, one in `chat`, one in a
// helper — and a front end that got the order wrong printed the answer on stderr, where a
// pipe could not see it. The rule is checked here once, against the function all three now
// call, so a fourth front end inherits it by calling the same thing.
//

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#include "pu/runtime/answer.hpp"

namespace {

using pu::ExecutionResult;

// A result built from the four facts the printers read, so a case states only the ones it is
// about.
ExecutionResult MakeResult(std::string content, bool streamed) {
  ExecutionResult result;
  result.content = std::move(content);
  result.was_streamed = streamed;
  return result;
}

}  // namespace

TEST_CASE("An answer ends on its own line", "[answer]") {
  // Printed to a terminal, the newline terminates the line; appended to a transcript, it is
  // what keeps one answer from running into the next.
  {
    std::ostringstream out;
    pu::runtime::PrintAnswer(MakeResult("hello", false), out);
    REQUIRE(out.str() == "hello\n");
  }

  // A streamed answer already reached the caller a token at a time, so what it still owes is
  // the line ending — not the text again, which would double it.
  {
    std::ostringstream out;
    pu::runtime::PrintAnswer(MakeResult("hello", true), out);
    REQUIRE(out.str() == "\n");
  }

  // A turn that said nothing still ends the line: a prompt followed by a silent turn followed
  // by the next prompt would otherwise read as one line.
  {
    std::ostringstream out;
    pu::runtime::PrintAnswer(MakeResult("", false), out);
    REQUIRE(out.str() == "\n");
  }
}

TEST_CASE("A command's own output is printed once and unspaced", "[answer]") {
  // `pu ask "/help"` was answered by the router, so `content` is that answer. It is not a
  // reply that ended in a newline already: adding one either way would change a table.
  ExecutionResult result = MakeResult("commands:\n  ask  ...", false);
  result.was_command = true;

  std::ostringstream out;
  pu::runtime::PrintAnswer(result, out);
  REQUIRE(out.str() == "commands:\n  ask  ...\n");

  // An empty answer from a command is not a blank line: the command was asked and answered,
  // and there is nothing to report.
  std::ostringstream empty_out;
  result.content.clear();
  pu::runtime::PrintAnswer(result, empty_out);
  REQUIRE(empty_out.str().empty());
}

TEST_CASE("A failed turn prints nothing on the answer stream", "[answer]") {
  // The point of the split: a caller redirecting stdout gets the answer, and a failure is not
  // an answer. A turn that failed after streaming part of a reply keeps the part it printed —
  // that already went out — but adds nothing to stdout.
  ExecutionResult result = MakeResult("half a repl", true);
  result.has_error = true;
  result.error_message = "provider refused";

  std::ostringstream out;
  pu::runtime::PrintAnswer(result, out);
  REQUIRE(out.str().empty());

  std::ostringstream err;
  pu::runtime::PrintFailure(result, err);
  REQUIRE(err.str() == "provider refused\n");
}

TEST_CASE("A turn that did not fail says nothing on the error stream", "[answer]") {
  // Otherwise every successful `pu ask` would carry an empty line on stderr, and a script
  // checking for one would read success as failure.
  ExecutionResult result = MakeResult("hello", false);
  result.error_message = "a message nobody should print";

  std::ostringstream err;
  pu::runtime::PrintFailure(result, err);
  REQUIRE(err.str().empty());
}
