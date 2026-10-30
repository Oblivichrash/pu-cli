// SPDX-License-Identifier: GPL-3.0-only
// The page's mutable facts, in one place. Every module reads and writes `PU`; none of
// them owns a private copy, so a fact has exactly one home.

"use strict";

const MAX_BUSY_RETRIES = 2;

// How much context each `gateway · model` holds, as the server last reported it. A window is a
// fact about a model rather than about a gateway — two gateways serve the same id at different
// sizes — so the key carries both. Empty means nobody stated one, and a blank bar is the honest
// answer there: NIM reports no window at all, and a nominal figure a product does not enforce
// would draw a fill that lies.
const CONTEXT_WINDOWS = {};

// The server this page is attached to. There is always exactly one, because a page was served by
// one: `origin` is the address that answered, `name` is the label it gave itself, and `port` is
// the one it answered on. None of the three is derived from the server list, which describes the
// others — a page that learned its own identity from a list would be blank whenever the list is
// unavailable, and the list is unavailable on every remote page by design.
const SELF = {
  origin: window.location.origin,
  name: "",
  port: null,
};

const PU = {
  ws: null,
  self: SELF,
  // The other servers on this machine, as `GET /api/servers` reports them: `{name, port}`. Empty is
  // an ordinary state, so the header draws the page's own name and offers no switch. Use
  // `setOthers`; a module that assigns the array directly leaves the header describing a list it
  // no longer has.
  others: [],
  // Why `others` is empty, when it is: `null` for "there are none", `"loopback"` for "this route is
  // answered on the machine the server runs on", and `"unreachable"` for a network failure. The
  // header says which, because a reader on another machine deserves to know the switcher is not
  // hidden by a bug.
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
  gatewayShown: "",
  backendModelShown: "",
  backendHostShown: "",
  // What the last reply ran against, as the bar prints it; empty until a turn reports usage.
  contextShown: "",
  // The prompt of the last turn, kept so the bar can be redrawn when the window arrives late:
  // the counts come with a turn and the window comes with the session, so the two are drawn
  // from state rather than from whichever payload happened to land last.
  lastTurnPrompt: null,
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

// The origin of another server on this machine. It is not built from that server's bind address:
// `0.0.0.0` is a legal place to listen and never an address to dial, and a server behind a proxy
// or another interface would give one this page cannot reach. A sibling is the port on the same
// host this page already reached, which is the one fact the two are guaranteed to share.
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
