// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// How a path written in a config is read against the workspace that configures it.
//
// One rule, in one place: a sandbox root and a delegate's directory are both written relative
// to the workspace and both end up in a tool, so the report that says where a tool will be
// allowed to write and the tool itself have to agree. They agreed by copy before this existed,
// which is a second rule waiting to be a different one.
//

#include <filesystem>
#include <string>

namespace pu {

// An absolute path is taken as written. Anything else — including nothing at all, which is what
// a file that names no root writes — is a path under `root`.
std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& root,
                                           const std::string& configured_path);

}  // namespace pu
