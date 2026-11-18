// SPDX-License-Identifier: GPL-3.0-only
#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include "pu/model/gateway.hpp"

// The table that says which provider a gateway opens is written above the code that reads it,
// because a provider is `llm`'s type and the rows name one. A program installs it once at its
// own entry point; the runner is this binary's entry point, so it does the same thing here
// rather than leaving every case to read an empty table.
int main(int argc, char* argv[]) {
  pu::model::InstallGatewayTable();
  return Catch::Session().run(argc, argv);
}
