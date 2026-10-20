// SPDX-License-Identifier: GPL-3.0-only
// The page's mutable facts, in one place. Every module reads and writes `PU`; none of
// them owns a private copy, so a fact has exactly one home.

"use strict";

const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  TEXT: "text",
};

const MAX_BUSY_RETRIES = 2;

const CONTEXT_WINDOWS = {};

const PU = {
  ws: null,
  // The server this page is talking to. Everything the page asks for is asked of it: the REST
  // calls read it, the socket is built from it. Empty until the page learns its own origin, so
  // a page opened on any server starts out talking to the one that served it.
  origin: "",
  // What `GET /api/servers` reported, and the entry this page is attached to: the page draws
  // the list from the first and calls the second the current one. Use `setServers` to write
  // them; a module that assigns either directly leaves the other describing a different server.
  servers: [],
  server: null,
  // A module loaded before notice.js cannot name showNotice. app.js points this at it
  // once every module is in; a remark that arrives before then is dropped.
  announce: () => {},
  isStreaming: false,
  stopRequested: false,
  reconnectAttempts: 0,
  reconnectTimer: null,
  connState: "connecting",
  replyLostToDisconnect: false,
  busyRefusals: 0,
  watchdogTimer: null,
  // Set once the watchdog has already given a running command its longer wait.
  watchdogToolGrace: false,
  // The socket a reopened one replaces. It keeps delivering the turn until the server
  // closes it, and its close is the handover rather than the page going away.
  supersededSocket: null,
  // True from a socket's open until the transcript it bought is rebuilt. Frames arriving
  // meanwhile wait in loadingFrames instead of being drawn into boxes about to be replaced.
  // Exactly one function raises it and two lower it: the rebuild's own end, and this timer.
  loading: false,
  loadingTimer: null,
  loadingFrames: [],
  // Bumped by each load, so a load that a later one overtook knows not to replay for it.
  loadGen: 0,
  // True while resyncAfterReconnect is rebuilding, so loadHistory leaves the flag to it.
  resyncing: false,
  // True while loadHistory is appending, so two runs cannot draw the chain twice.
  loadHistBusy: false,

  // The next message sent is turn chainLength + 1, which is the number /api/rewind expects.
  chainLength: 0,
  currentAssistantBlocks: [],
  currentAssistantEl: null,
  // name -> description, so the closed control can carry the chosen agent's tooltip.
  agentDescriptions: {},
  backendTypeShown: "",
  backendModelShown: "",
  backendHostShown: "",
  // What the last reply ran against, as the bar prints it; empty until a turn reports usage.
  contextShown: "",
  // The last total the server reported for this conversation, kept so a failed read leaves
  // the panel as it was rather than blanking a figure that is still true.
  usageTotal: null,

  composer: {
    composing: false,
    inputEl: null,
    sendBtn: null,
    messagesEl: null,
    agentSelect: null,
    agentChangeHandler: null,
    // The settings the server offered, as it last reported them: the rows are drawn from this
    // and the status bar reads it, so a setting added on the server needs no change here.
    settings: [],
    settingsRows: null,
    settingsReset: null,
  },
};

// The one place `PU.servers`, `PU.server` and `PU.origin` are written. They describe one
// fact — which server this page is talking to — and a page that wrote two of the three would
// be drawing a header from one server and asking the other for its transcript. Naming them
// together is what makes that impossible rather than a rule to remember.
//
// `server` may be null (a server the registry has not listed), which is why the origin is
// derived here: the page is attached to the address it was opened on, and the label is the
// only thing missing. A caller that leaves an argument out is not changing that part.
function setServers({ servers, server }) {
  if (servers !== undefined) PU.servers = Array.isArray(servers) ? servers : [];
  if (server !== undefined) {
    PU.server = server || null;
    PU.origin = server ? serverOrigin(server) : window.location.origin;
  }
}

function setConnectionState(state) {
  if (state === PU.connState) return;
  const dot = document.getElementById("conn-dot");
  const text = state === "online" ? "Connected"
    : (state === "offline" ? "Disconnected" : "Reconnecting…");
  if (dot) {
    dot.className = "conn-dot " + state;
    dot.title = text;
  }
  const label = document.getElementById("conn-text");
  if (label) label.textContent = text;
  if (state === "offline" && PU.connState === "online" && PU.replyLostToDisconnect) {
    showNotice("Disconnected; the reply in progress was stopped and is not kept.");
  }
  if (state === "online" && PU.connState === "offline") hideNotice();
  PU.replyLostToDisconnect = false;
  PU.connState = state;
}

function contextWindowFor(backendType) {
  return CONTEXT_WINDOWS[backendType] || null;
}

function createSystemMessage(text) {
  const el = document.createElement("div");
  el.className = "msg system";
  el.textContent = text;
  PU.composer.messagesEl.appendChild(el);
  return el;
}

// Read from the element each time rather than cached, because content grows under the
// reader: a stored answer goes stale the moment a bubble is appended and the reader has
// not moved, and a stale "not at the bottom" then stops the reply following for good.
function isAtBottom() {
  const el = PU.composer.messagesEl;
  return el.scrollHeight - el.scrollTop - el.clientHeight < 10;
}

function scrollToBottom() {
  const el = PU.composer.messagesEl;
  el.scrollTop = el.scrollHeight;
}
