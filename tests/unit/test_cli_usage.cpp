// SPDX-License-Identifier: GPL-3.0-only

//
// `pu ask --show-usage`: what the turn cost, on stderr, after the answer.
//
// stdout is the answer and stays the answer — a cost printed beside it would make the reply
// the first field of a record nobody agreed to. The line itself is what a caller reads, so
// these cases hold its shape: what it names, what it leaves out, and where it is written.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <iostream>
#include <sstream>
#include <string>

#include "pu/app/cli.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"
#include "tests/mocks/serve_harness.hpp"

namespace {

// Captures one standard stream while a command writes to it.
class CapturedStream {
 public:
  explicit CapturedStream(std::ostream& stream)
      : stream_(stream), old_(stream.rdbuf(buffer_.rdbuf())) {}
  ~CapturedStream() { stream_.rdbuf(old_); }
  std::string text() const { return buffer_.str(); }

 private:
  std::ostream& stream_;
  std::ostringstream buffer_;
  std::streambuf* old_;
};

}  // namespace

TEST_CASE("--show-usage writes the cost to stderr and leaves stdout to the answer",
          "[cli][usage]") {
  pu::tests::ServeHarness harness;

  // The harness's runtime already points at its fake backend, which reports a usage object.
  CapturedStream out(std::cout);
  CapturedStream err(std::cerr);
  const int status =
      pu::cli::RunAsk("chat", "hello", harness.RuntimeRef(), /*show_usage=*/true);

  REQUIRE(status == pu::cli::kOk);
  REQUIRE(err.text().find("usage:") != std::string::npos);
  // The counts the backend reported, named in the order a reader expects.
  REQUIRE(err.text().find("prompt") != std::string::npos);
  REQUIRE(err.text().find("completion") != std::string::npos);
  // stdout carries the answer, and the cost is not in it: a caller piping the reply gets the
  // reply.
  REQUIRE(out.text().find("usage:") == std::string::npos);
}

TEST_CASE("Without the flag the cost is not printed at all", "[cli][usage]") {
  pu::tests::ServeHarness harness;

  CapturedStream err(std::cerr);
  const int status =
      pu::cli::RunAsk("chat", "hello", harness.RuntimeRef(), /*show_usage=*/false);

  REQUIRE(status == pu::cli::kOk);
  REQUIRE(err.text().find("usage:") == std::string::npos);
}
