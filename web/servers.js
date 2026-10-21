// SPDX-License-Identifier: GPL-3.0-only
// The servers running on this machine, and the one this page is talking to. The header names the
// workspace this page is attached to, and lists the others when there are any.
//
// Two facts, two homes. The page's own name comes from the server that answered (`/api/session`),
// because only that server knows which directory it serves and a path is not something it hands a
// remote reader. The list comes from `/api/servers`, which answers on the machine the server runs
// on: a remote page gets a refusal there, and that refusal is an ordinary state the header draws
// rather than an error the page hides.
//
// Switching is light on purpose. One socket, one transcript, one board: the page rebuilds
// against the server it moved to and lets the one it left keep running whatever it was running. A
// turn that was streaming is handed over rather than cancelled, so switching back finds it where
// it was.

"use strict";

const SERVERS_POLL_MS = 5000;

// The workspace a server serves, as a label the header can carry. The name arrives computed; this
// only falls back for an entry that somehow has none.
function serverLabel(server) {
  return server.name || `:${server.port}`;
}

function currentServerPort() {
  return PU.self.port;
}

function renderServers() {
  const toggle = document.getElementById("server-toggle");
  const list = document.getElementById("server-list");
  const name = document.getElementById("server-current");
  if (!toggle || !list || !name) return;

  // The name is always known: this page was served by a server, and that server names itself. It
  // does not wait for the list, and it is not derived from the address, because a page opened
  // through a proxy or on another machine has an address that is not the server's name.
  name.textContent = PU.self.name || PU.self.origin;
  name.title = PU.self.name ? `${PU.self.name} — ${PU.self.origin}` : PU.self.origin;
  name.disabled = PU.others.length < 1;

  // Why the caret cannot be used is worth one sentence, and it is not the same sentence in each
  // case: a machine with one server is not the same as a route this page may not read.
  const unavailable = PU.othersRestricted === "loopback"
    ? "A machine's server list is read on the machine itself. This page reached one over the network."
    : (PU.othersRestricted === "unreachable" ? "The server list could not be read." : "");

  list.replaceChildren();
  toggle.hidden = false;
  toggle.disabled = PU.others.length < 1;
  toggle.title = unavailable || (PU.others.length < 1
    ? "No other server is running on this machine"
    : "Other servers on this machine");
  if (PU.others.length < 1) list.hidden = true;

  PU.others.forEach((server) => {
    const item = document.createElement("li");
    item.className = "server-row";
    item.title = `${serverLabel(server)} — ${siblingOrigin(server.port)}`;

    const label = document.createElement("button");
    label.type = "button";
    label.className = "server-name";
    label.textContent = serverLabel(server);
    label.addEventListener("click", () => {
      list.hidden = true;
      switchToServer(server);
    });
    item.appendChild(label);

    list.appendChild(item);
  });
}

// The list, read on its own clock. A refusal is an answer: the route is loopback-only, so a remote
// page is told so and the header says why it has nothing to offer. Either way the header is drawn,
// which is the difference between a control that is unavailable and a control that is missing.
async function refreshServers() {
  let servers = [];
  let restricted = null;
  try {
    const data = await fetchJson("/api/servers");
    servers = Array.isArray(data.servers) ? data.servers : [];
  } catch (error) {
    // `403` is the route saying it answers this machine only; anything else is a server this page
    // could not reach, which the connection dot already reports. Both are drawn, not swallowed.
    restricted = error && error.status === 403 ? "loopback" : "unreachable";
  }
  setOthers({ servers, restricted });
  renderServers();
}

// Everything the page has drawn is about one server, so it is dropped rather than patched. The
// transcript, the session and the board are read again from the one it moved to; nothing is
// carried across, because nothing about a conversation in one workspace is true of another.
async function adoptServer(server) {
  setSelf({ origin: siblingOrigin(server.port), name: serverLabel(server), port: server.port });
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
  // The session answers with the name and port the page is now attached to, so a switch ends by
  // correcting the label from the server it arrived at rather than trusting the entry it used.
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
  renderServers();
  refreshServers();
  setInterval(refreshServers, SERVERS_POLL_MS);
}
