// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// The exit codes the standalone tools share.
//
// `pu` has more of them — a usage error, a sandbox that refused — and the ones here are the
// subset a client of a stream can reach. This list lives in its own header because more than
// one tool answers with it: a copy per header is two descriptions of one set of numbers.
//

namespace pu::tools_cli {

enum ExitCode : int {
  kOk = 0,
  kUsage = 1,
  kProvider = 2,
  kInterrupted = 4,
};

}  // namespace pu::tools_cli
