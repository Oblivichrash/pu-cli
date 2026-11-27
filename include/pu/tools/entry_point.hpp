// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// The shell every standalone tool's `main` is made of.
//
// `pu-probe`, `pu-models` and `pu-sse` each open a stream of work the same way: parse the
// command line, answer `--help`, refuse a bare invocation that would otherwise reach the
// network, run the tool, and turn the three things that can go wrong into an exit code. Written
// three times that is three chances to disagree; one of the copies already did, and the early
// return it grew was invisible because the other copy had the same one.
//
// So the order of the checks lives here, once. A tool supplies its options and what to do once
// they are parsed, and nothing else.
//

#include <boost/program_options.hpp>

#include <exception>
#include <optional>
#include <iostream>
#include <string>
#include <utility>

#include "pu/core/text.hpp"
#include "pu/tools/exit_code.hpp"

namespace pu::tools {

// How a tool opens: its own option list, and whether a bare invocation is a request for the
// help page. A bare `pu-probe` would otherwise probe whatever agent happens to be the default,
// which is a network call nobody asked for, so saying what the tool takes is the answer to
// "what does this do" — `pu-sse` asks for a URL instead, and says so in its own body.
enum class BareInvocation {
  kPrintHelp,
  kRun,
};

namespace detail {

// The catch order matters and is the reason it is written once: an unreachable provider is the
// case a retry can fix, so it is reported as `kProvider` and not as a usage error. `kUsage` is
// the last resort for anything a tool threw that it did not classify itself.
template <typename Body>
int RunAndClassify(Body&& body) {
  try {
    return body();
  } catch (const pu::HttpError& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kProvider);
  } catch (const std::exception& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }
}
}  // namespace detail

// Parses `argv`, answers `--help` and a bare invocation, then hands `vm` to `body`.
//
// `extend` runs after the shared checks and before `body`, for the parsing a tool does not want
// to write into a `boost::program_options` description: `pu-sse` needs a URL before it can do
// anything, and `pu-models` needs `--detail` to name one of three shapes. Either may return an
// exit code to stop there, or nothing to carry on.
template <typename Extend, typename Body>
int RunEntryPoint(int argc, char** argv, const boost::program_options::options_description& options,
                  BareInvocation bare, Extend&& extend, Body&& body) {
  namespace po = boost::program_options;

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
    po::notify(vm);
  } catch (const std::exception& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }

  if (vm.count("help") > 0) {
    std::cout << options << "\n";
    return static_cast<int>(ExitCode::kOk);
  }

  if (argc <= 1 && bare == BareInvocation::kPrintHelp) {
    std::cout << options << "\n";
    return static_cast<int>(ExitCode::kOk);
  }

  if (auto stopped = extend(vm); stopped.has_value()) return *stopped;

  return detail::RunAndClassify([&] { return body(vm); });
}

}  // namespace pu::tools
