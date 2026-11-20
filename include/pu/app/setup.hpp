// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>

#include "pu/config/setup.hpp"

namespace pu::cli {

// Asks the questions a workspace with no agents.json cannot answer for itself -- gateway, URL,
// key, model -- and returns what the reader chose, or `nullptr` when they gave no usable
// answer or the file is already there.
//
// It asks and returns; writing is `config::WriteSetupConfig`, so a caller can print the file
// instead of storing it, and a test can check the answers without a workspace at all.
//
// Prompts go to stderr and the answers are read from stdin, which leaves stdout for whatever
// the command prints once the wizard is done.
std::optional<config::SetupChoice> RunSetupWizard(const std::filesystem::path& workspace);

}  // namespace pu::cli
