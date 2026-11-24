// SPDX-License-Identifier: GPL-3.0-only
// The page's mutable facts, in one place. Every module reads and writes `PU`; none of
// them owns a private copy, so a fact has exactly one home.

"use strict";

const MAX_BUSY_RETRIES = 2;

// How much context each `gateway · model` holds, as the server last reported it. The key carries
// both because a window is a fact about a model, and two gateways serve the same id at different
// sizes. Empty means nobody stated one, and a blank bar is the honest answer: NIM reports no
// window at all, and a nominal figure a product does not enforce would draw a fill that lies.
const CONTEXT_WINDOWS = {};

// The server this page is attached to: `origin` answered, `name` is the label it gave itself,
// `port` is where it answered. None of the three is derived from the server list, which describes
// the others — a page that learned its own identity from a list would be blank on every remote
// page, where that list is unavailable by design.
const SELF = {
  origin: window.location.origin,
  name: "",
  port: null,
};

const PU = {
  ws: null,
  self: SELF,
  // The other servers on this machine, as `GET /api/servers` reports them: `{name, port}`. Use
  // `setOthers`: assigning the array directly leaves the header describing a list it no longer has.
  others: [],
  // Why `others` is empty, when it is: `null` for "there are none", `"loopback"` for "this route is
  // answered on the machine the server runs on", and `"unreachable"` for a network failure.
  othersRestricted: null,
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
  // Lowered by two things: the rebuild's own end, and this timer.
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
  // The message being edited, as `{el, turn, original}`, or null. The words live in the composer,
  // not the bubble: an edit is a sentence about to be said again.
  editing: null,
  currentAssistantBlocks: [],
  currentAssistantEl: null,
  gatewayShown: "",
  // Each gateway's default address, from /api/gateways. Read when the menu changes, so the
  // address box follows the gateway rather than keeping the one just left behind.
  gatewayHosts: {},
  backendModelShown: "",
  // Which control in the model form holds the name: the menu when the catalogue had entries, a
  // typed box when it had none. Set by the fill that shows one and hides the other, so Save reads
  // the box the reader is actually looking at rather than re-deriving that from the DOM.
  modelTyped: false,
  backendHostShown: "",
  // The sampling settings in force, as `/api/backend` states them. A `null` member is a setting
  // nobody named, which is not zero: the model form draws it as an empty box, and clearing a box
  // sends the null back rather than a number invented here.
  backendParamsShown: {
    temperature: null,
    max_tokens: null,
    top_p: null,
    seed: null,
    context_window: null,
  },
  // What the last reply ran against, as the bar prints it; empty until a turn reports usage.
  contextShown: "",
  // Kept so the bar can be redrawn when the window arrives late: the counts come with a turn and
  // the window comes with the session, so both are drawn from state rather than from whichever
  // payload landed last.
  lastTurnPrompt: null,
  // That same turn as the console stated it -- count, cached share, completion -- so the tooltip
  // repeats it rather than recomputing it.
  lastTurnLine: "",
  usageTotal: null,

  composer: {
    composing: false,
    // "new" while the field is a message being typed, "edit" while it holds one being replaced.
    // The button reads this, so the two jobs can share a field without sharing a label.
    mode: "new",
    inputEl: null,
    sendBtn: null,
    messagesEl: null,
    gatewaySelect: null,
    // The settings the server offered, as it last reported them: the rows are drawn from this
    // and the status bar reads it, so a setting added on the server needs no change here.
    settings: [],
    settingsRows: null,
    settingsReset: null,
  },
};

// Not built from the sibling's bind address: `0.0.0.0` is a legal place to listen and never an
// address to dial, and a server behind a proxy would give one this page cannot reach.
function siblingOrigin(port) {
  const scheme = window.location.protocol === "https:" ? "https" : "http";
  return `${scheme}://${window.location.hostname}:${port}`;
}

// The one place `PU.others` is written. The label a reader sees for this page is not here: it is
// `PU.self.name`, which the server that answered supplied.
function setOthers({ servers, restricted }) {
  if (servers !== undefined) PU.others = Array.isArray(servers) ? servers : [];
  if (restricted !== undefined) PU.othersRestricted = restricted || null;
}

// The one place `PU.self` is written. Switching servers replaces all three fields together, so a
// page cannot end up asking one server for a transcript it labels with another's name.
function setSelf({ origin, name, port }) {
  if (origin !== undefined) PU.self.origin = origin || window.location.origin;
  if (name !== undefined) PU.self.name = name || "";
  if (port !== undefined) PU.self.port = port === null ? null : Number(port);
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

function contextWindowKey(gateway, model) {
  return `${gateway || ""} · ${model || ""}`;
}

function contextWindowFor(gateway, model) {
  return CONTEXT_WINDOWS[contextWindowKey(gateway, model)] || null;
}

// The window a server stated for the model it is running. `0` is "nobody said one", which is
// answered by forgetting the model rather than by storing a zero: a figure that is absent and a
// figure of zero are different answers, and only the first should leave the bar blank.
function setContextWindow(gateway, model, window) {
  const key = contextWindowKey(gateway, model);
  if (Number.isFinite(window) && window > 0) {
    CONTEXT_WINDOWS[key] = window;
  } else {
    delete CONTEXT_WINDOWS[key];
  }
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
