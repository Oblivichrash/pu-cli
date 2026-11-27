// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include "pu/core/platform.hpp"
#include "pu/model/gateway.hpp"

// The process starts here and decides nothing else. Two things must happen before any
// command can run — the signal handler, so an interrupt reaches a turn, and the gateway
// table, so a lookup below `llm` finds a row — and after them the line is `cli`'s.
int main(int argc, char* argv[]) {
  pu::platform::SetupSignalHandler();

  // The rows that name a provider are written in `pu_llm`, above the module that reads them.
  // Installing them here, once, is what lets a lookup below `llm` find a table at all.
  pu::model::InstallGatewayTable();

  return pu::cli::RunCommandLine(argc, argv);
}
