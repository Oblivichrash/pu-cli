// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pu::config {

// One server's own account of itself: the directory it serves, the address it answers on, and
// the process that holds it. The page needs the first two to draw a list of servers, and the
// third only to tell a server that is still running from one that has stopped.
struct ServeRecord {
  std::string directory;
  std::string host;
  std::uint16_t port = 0;
  int pid = 0;
};

// The file every server on this machine writes itself into and deletes itself out of. It sits
// beside the user's agents.json because it describes the machine rather than a workspace: the
// point of it is to be found from another directory than the one it describes.
std::string ServeRegistryPath();

// The servers that are still running, highest port first. A record whose process has stopped is
// not returned, and is removed from the file on the way out, so a crash costs nothing but a stale
// line that the next reader deletes.
std::vector<ServeRecord> LiveServeRecords();

// Writes this server's record, replacing any earlier one for the same process. Returns the path
// written, or empty when the file could not be written, which is a degraded list and not a
// failed server.
std::string RegisterServeRecord(const ServeRecord& record);

void UnregisterServeRecord(int pid);

}  // namespace pu::config
