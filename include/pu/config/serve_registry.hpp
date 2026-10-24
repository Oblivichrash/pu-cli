// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pu::config {

// One server's own account of itself: the directory it serves, the address it listens on, and
// the process that holds it. `bind` is where the listener sits, which is not the address a
// browser dials: a server bound to every interface answers on one the machine happens to have,
// and the page that reached it already knows which one that was.
//
// `started` is the process's own start time, and it exists because a pid is only unique among the
// processes running now: once its owner exits the number is handed to the next program, so a
// record that stored a pid and asked later whether "the process" was alive would get a true
// answer about something else, and the page would list a server nobody can reach.
struct ServeRecord {
  std::string directory;
  std::string bind;
  std::uint16_t port = 0;
  int pid = 0;
  std::string started;
};

// Whether the record still describes a running server: the pid is alive and it is the same
// process the record was written by. An empty stamp on either side means the platform could not
// say, and the pid is all that is left to go on, which is the answer that was given before
// stamps existed.
bool StillRunning(const ServeRecord& record);

// The file every server on this machine writes itself into and deletes itself out of. It sits
// beside the user's agents.json because it describes the machine rather than a workspace: the
// point of it is to be found from another directory than the one it describes.
std::string ServeRegistryPath();

// The servers that are still running, highest port first. A record whose process has stopped, or
// whose pid now belongs to a different program, is not returned and is removed from the file on
// the way out, so a crash or a reused pid costs nothing but a stale line the next reader deletes.
std::vector<ServeRecord> LiveServeRecords();

// The records that are not `pid`'s own. A page lists what it could switch to, so the server that
// answered is the one entry the list must never carry: it is already open, and naming it again
// would offer a switch to where the reader already is.
std::vector<ServeRecord> OthersAmong(const std::vector<ServeRecord>& records, int pid);

// The name a workspace is known by on the page: the last two segments of its directory, so a
// project is named without repeating where it lives. The server computes it because the page
// must never see a path a reader's own machine did not choose.
std::string WorkspaceLabel(const std::string& directory);

// Writes this server's record, replacing any earlier one for the same process. Returns the path
// written, or empty when the file could not be written, which is a degraded list and not a
// failed server.
std::string RegisterServeRecord(const ServeRecord& record);

void UnregisterServeRecord(int pid);

}  // namespace pu::config
