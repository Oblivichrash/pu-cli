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
  const scheme = window.location.protocol === "https:" ? "https" : "http";
  return `${scheme}://${server.host}:${server.port}`;
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

  // The name is the workspace, and the page is always attached to one: a server that has not
  // registered itself yet is still the server that answered, so it is named from the address
  // the page was opened on rather than left as the placeholder.
  const here = PU.server || { directory: "", host: window.location.hostname, port: window.location.port };
  name.textContent = here.directory ? serverLabel(here) : serverOrigin(here);
  name.title = here.directory || serverOrigin(here);
  name.disabled = PU.servers.length < 2;

  list.replaceChildren();
  // The caret stays: a machine that gains a second server later has one place to look, and a
  // control that appears only once there are two is a control nobody can find. The list itself
  // opens only when there is something in it to switch to.
  toggle.hidden = false;
  toggle.disabled = PU.servers.length < 2;
  if (PU.servers.length < 2) list.hidden = true;

  const current = currentServerIndex();
  PU.servers.forEach((server, index) => {
    const item = document.createElement("li");
    item.className = "server-row" + (index === current ? " current" : "");
    item.title = server.directory || "";

    const label = document.createElement("button");
    label.type = "button";
    label.className = "server-name";
    label.textContent = serverLabel(server);
    label.disabled = index === current;
    label.addEventListener("click", () => {
      list.hidden = true;
      switchToServer(server);
    });
    item.appendChild(label);

    list.appendChild(item);
  });
}

async function refreshServers() {
  let servers;
  try {
    const data = await fetchJson("/api/servers");
    servers = Array.isArray(data.servers) ? data.servers : [];
  } catch (_) {
    return;
  }
  // The server that just answered is the one this page is attached to, whether or not it says
  // so: a page that was opened on a server the registry has not caught up with still knows
  // where it is, and a page that switched has already named the entry it asked for.
  const server = PU.server || servers.find((entry) => entry.current) || null;
  setServers({ servers, server });
  renderServers();
}

// Everything the page has drawn is about one server, so it is dropped rather than patched. The
// transcript, the session and the board are read again from the one it moved to; nothing is
// carried across, because nothing about a conversation in one workspace is true of another.
async function adoptServer(server) {
  setServers({ server });
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
