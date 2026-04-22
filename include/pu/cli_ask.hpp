// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// CLI entry point for the 'ask' subcommand.

#pragma once

namespace pu::cli {

// Run the 'ask' subcommand. argc is the argument count (including the
// subcommand name), argv is the argument vector where argv[0] is "ask".
// Returns 0 on success, non-zero on error.
int RunAskCommand(int argc, char* argv[]);

}  // namespace pu::cli
