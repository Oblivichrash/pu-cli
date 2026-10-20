// SPDX-License-Identifier: GPL-3.0-only
// The servers running on this machine, and the one this page is talking to. The point of the
// list is to name a workspace instead of a port: nothing here asks a reader to remember which
// number belongs to which directory, because the server that serves a directory is the one
// that says so.
//
// Switching is light on purpose. One socket, one transcript, one board: the page rebuilds
// against the server it moved to and lets the one it left keep running whatever it was
// running. A turn that was streaming is handed over rather than cancelled, so switching back
// finds it where it was.

"use strict";

const SERVERS_POLL_MS = 5000;

function serverOrigin(server) {
  return `http://${server.host}:${server.port}`;
}

// The workspace a server serves, as the page names it: the last two segments of the directory
// say which project this is without repeating where it lives, and the whole path is the
// tooltip for the case where two projects share a name.
function serverLabel(server) {
  const parts = String(server.directory || "").split(/[\\/]+/).filter((part) => part !== "");
  if (parts.length === 0) return server.directory || "unknown";
  return parts.slice(-2).join("/");
}

function currentServerIndex() {
  const servers = PU.servers;
  for (let i = 0; i < servers.length; i += 1) {
    if (PU.server && servers[i].port === PU.server.port) return i;
  }
  return -1;
}

function renderServers() {
  const toggle = document.getElementById("server-toggle");
  const list = document.getElementById("server-list");
  const name = document.getElementById("server-current");
  if (!toggle || !list || !name) return;

  if (PU.server) {
    name.textContent = serverLabel(PU.server);
    name.title = PU.server.directory || "";
  }

  list.replaceChildren();
  // One server is the ordinary case, and a list of one says nothing the header's name does
  // not. The list appears when a second server is there to switch to.
  toggle.hidden = PU.servers.length < 2;
  if (toggle.hidden) list.hidden = true;

  const here = currentServerIndex();
  PU.servers.forEach((server, index) => {
    const item = document.createElement("li");
    item.className = "server-row" + (index === here ? " current" : "");
    item.title = server.directory || "";

    const label = document.createElement("button");
    label.type = "button";
    label.className = "server-name";
    label.textContent = serverLabel(server);
    label.disabled = index === here;
    label.addEventListener("click", () => {
      list.hidden = true;
      switchToServer(server);
    });
    item.appendChild(label);

    list.appendChild(item);
  });
}

async function refreshServers() {
  try {
    const data = await fetchJson("/api/servers");
    PU.servers = Array.isArray(data.servers) ? data.servers : [];
    // The server that just answered is the one this page is attached to, whether or not it
    // says so: a page that was opened on a server the registry has not caught up with still
    // knows where it is, and a page that switched knows which entry it asked for.
    if (!PU.server) PU.server = PU.servers.find((server) => server.current) || null;
  } catch (_) {
    return;
  }
  renderServers();
}

// Everything the page has drawn is about one server, so it is dropped rather than patched. The
// transcript, the session and the board are read again from the one it moved to; nothing is
// carried across, because nothing about a conversation in one workspace is true of another.
async function adoptServer(server) {
  PU.origin = server ? serverOrigin(server) : window.location.origin;
  PU.server = server || null;
  if (PU.ws) {
    PU.ws.onmessage = null;
    PU.ws.onclose = null;
    PU.ws.close();
    PU.ws = null;
  }
  releaseSupersededSocket();
  PU.composer.messagesEl.replaceChildren();
  PU.isStreaming = false;
  setSendButtonState(false);
  renderServers();
  await loadAgents();
  await loadSession();
  await loadHistory();
  scrollToBottom();
  connectWebSocket();
  refreshBoard();
}

async function switchToServer(server) {
  if (PU.isStreaming) {
    PU.announce("This server is still answering. Let the turn finish before switching.");
    return;
  }
  await adoptServer(server);
}

function startServerPolling() {
  const toggle = document.getElementById("server-toggle");
  const list = document.getElementById("server-list");
  if (toggle && list) {
    toggle.addEventListener("click", () => {
      list.hidden = !list.hidden;
    });
  }
  refreshServers();
  setInterval(refreshServers, SERVERS_POLL_MS);
}
