// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// CLI entry point for the 'chat' subcommand (interactive mode).

#pragma once

namespace pu::cli {

// Run the 'chat' subcommand. argc includes the subcommand name, argv[0] is "chat".
// Returns 0 on success, non-zero on error.
int RunChatCommand(int argc, char* argv[]);

}  // namespace pu::cli
