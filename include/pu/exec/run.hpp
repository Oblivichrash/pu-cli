// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "pu/core/base.hpp"

namespace pu::exec {

// Which end of a stream a bounded capture keeps. A log is read from its end and a file
// about to be edited from its start, so the budget follows the call rather than always
// paying for the opening bytes.
enum class Keep { kHead, kEnd };

// How much of a stream there was, and how much of it was kept. `from_end` says which
// part survived, which is what lets a page number its lines against the command that
// printed them instead of against the buffer that happened to hold them.
struct Extent {
  std::size_t lines = 0;     // what the command printed
  std::size_t bytes = 0;     // what the command printed
  std::size_t retained = 0;  // what the buffer kept
  bool from_end = false;

  bool truncated() const { return retained < lines; }
  // Lines the kept run sits behind. Only an end-kept capture drops any.
  std::size_t skipped() const { return from_end && lines > retained ? lines - retained : 0; }
};

// What a command left behind, apart from its output: the verdict a page repeats rather
// than reporting a success of its own.
struct Verdict {
  std::string command;
  bool ok = false;
  int exit_code = 0;
  std::string error;

  bool stopped = false;
};

// A command's output as it was kept.
struct Captured {
  std::string text;
  Extent extent;
};

// A command that has finished. The output and the verdict are one value because only
// the reader of the stream knows both: a reader that looked away has to say so, and an
// exit code means nothing without knowing whether the process was killed.
struct Ran {
  Verdict verdict;
  Captured captured;
};

// Runs a shell command with stderr merged into stdout. `max_bytes` of 0 keeps all. The
// budget is spent on the end `keep` names; the command still runs to its own end,
// because a reader that walked away would turn its exit code into a signal.
Ran Run(std::string_view command, const CancelToken& stop, std::string_view working_dir,
        std::size_t max_bytes, Keep keep = Keep::kHead);

}  // namespace pu::exec
