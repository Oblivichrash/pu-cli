// SPDX-License-Identifier: GPL-3.0-only
// The page's single socket and the REST calls that go with it: one page owns a session, so
// the page keeps exactly one socket and reports a refusal instead of taking the session
// from the page that holds it.

"use strict";

// A turn that says nothing for this long is treated as a dead socket. A command can run
// for minutes without a frame, so the wait is longer while one is running.
const WATCHDOG_STREAM_MS = 60000;
const WATCHDOG_TOOL_MS = 300000;

// The tab's name for itself. sessionStorage keeps it across a reload and gives every other
// tab its own, so the server can tell this page's reconnects from a second page.
function pageClientId() {
  const key = "pu-client-id";
  let id = null;
  try {
    id = window.sessionStorage.getItem(key);
  } catch (_) {}
  if (!id) {
    id = Math.random().toString(36).slice(2) + Date.now().toString(36);
    try {
      window.sessionStorage.setItem(key, id);
    } catch (_) {}
  }
  return id;
}

async function postJson(path, body) {
  const res = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return res.json();
}

async function fetchJson(path) {
  const res = await fetch(path);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

function sendFrame(type, payload) {
  if (!PU.ws || PU.ws.readyState !== WebSocket.OPEN) return false;
  PU.ws.send(JSON.stringify(payload === undefined ? { type } : { type, payload }));
  return true;
}

function scheduleReconnect() {
  if (PU.reconnectTimer) return;
  const delay = Math.min(500 * 2 ** PU.reconnectAttempts, 8000);
  PU.reconnectAttempts += 1;
  PU.reconnectTimer = setTimeout(() => {
    PU.reconnectTimer = null;
    connectWebSocket();
  }, delay);
}

function handleBusy(payload) {
  PU.busyRefusals += 1;
  const reason = (payload && payload.text) || "This session is already open in another page.";
  showNotice(PU.busyRefusals > MAX_BUSY_RETRIES
      ? reason + " Reload this page once it is free."
      : reason + " Trying again…");
}

function toolIsRunning() {
  return PU.currentAssistantBlocks.some(
      (block) => block.type === BLOCK_TYPES.TOOL_CALL && block.status === "running");
}

// A turn that stops speaking without a close is indistinguishable from a hung socket, so
// the socket is reopened. The timer is re-armed on every progress frame.
function armWatchdog() {
  clearTimeout(PU.watchdogTimer);
  // Progress arrived, so a running command is no longer silent.
  PU.watchdogToolGrace = false;
  PU.watchdogTimer = setTimeout(onWatchdogElapsed, WATCHDOG_STREAM_MS);
}

function onWatchdogElapsed() {
  PU.watchdogTimer = null;
  if (!PU.isStreaming) return;
  // A command that is still running says nothing until it ends, so the silence is not a
  // hang. It is given the longer wait once rather than indefinitely: a tool that died
  // without its `tool_end` must still lead to a reopened socket.
  if (toolIsRunning() && !PU.watchdogToolGrace) {
    PU.watchdogToolGrace = true;
    PU.watchdogTimer = setTimeout(onWatchdogElapsed, WATCHDOG_TOOL_MS);
    return;
  }
  reopenSilentSocket();
}

// The socket being given up on is handed over, not closed. Its close would reach the server
// as this page going away and cancel the turn still being written; instead the server closes
// it once the replacement has taken the session. Until then its frames keep being drawn,
// since they are this turn's, and its close is ignored: that close is the handover rather
// than the page leaving.
function reopenSilentSocket() {
  PU.watchdogTimer = null;
  PU.watchdogToolGrace = false;
  if (!PU.isStreaming) return;
  // Only one socket can be waiting to be closed by the server: an older one has had its
  // turn handed on already.
  releaseSupersededSocket();
  const stale = PU.ws;
  PU.ws = null;
  if (stale) {
    PU.supersededSocket = stale;
    stale.onopen = null;
    stale.onerror = null;
    stale.onclose = () => {
      if (PU.supersededSocket === stale) PU.supersededSocket = null;
    };
  }
  // This is a deliberate fresh connection, not a retry of a failed one.
  PU.reconnectAttempts = 0;
  connectWebSocket();
}

// Let go of the handed-over socket once its turn is over. Closing it while the turn runs
// would be read as the page leaving; closing it after `done` reaches a server with no turn
// in flight, which is why the release waits for the end of the turn.
function releaseSupersededSocket() {
  const stale = PU.supersededSocket;
  PU.supersededSocket = null;
  if (!stale) return;
  stale.onmessage = null;
  stale.onclose = null;
  stale.close();
}

function handleDone(payload) {
  // A turn that ends with nothing drawn leaves a blank bubble that reads as a hang;
  // name it instead of leaving the box empty.
  if (PU.currentAssistantEl && PU.currentAssistantBlocks.length === 0) {
    removeCurrentAssistantMessage();
    createSystemMessage("The reply came back empty. Try rephrasing, or check the backend.");
  }
  finishAssistantMessage();
  clearTimeout(PU.watchdogTimer);
  PU.watchdogTimer = null;
  PU.watchdogToolGrace = false;
  // The turn is over, so the socket it was handed over from has nothing left to deliver.
  releaseSupersededSocket();
  setSendButtonState(false);
  refreshChainLength();
  if (PU.stopRequested) {
    createSystemMessage("Stopped.");
    PU.stopRequested = false;
  }
  if (payload && payload.model) setBackendLabel(PU.backendTypeShown, payload.model);
}

function handleError(payload) {
  const errMsg = payload.text || "Unknown error";
  if (PU.currentAssistantEl) {
    PU.currentAssistantEl.classList.remove("typing");
    PU.currentAssistantEl.className = "msg error";
    PU.currentAssistantEl.innerHTML = errMsg;
    PU.currentAssistantEl = null;
    PU.currentAssistantBlocks = [];
  } else {
    createSystemMessage("Error: " + errMsg);
  }
  PU.stopRequested = false;
  // The turn is over, so the socket it was handed over from has nothing left to deliver.
  releaseSupersededSocket();
  setSendButtonState(false);
  refreshChainLength();
}

function handleNotice(payload) {
  const text = payload && payload.text ? payload.text : "";
  if (text) showNotice(text);
}

const FRAME_HANDLERS = {
  approval_ask: (payload) => showApprovalBar(payload.command),
  tool_start: handleToolStart,
  tool_end: handleToolEnd,
  chunk: handleChunk,
  thinking: handleThinking,
  done: handleDone,
  error: handleError,
  notice: handleNotice,
  usage: handleUsage,
  busy: handleBusy,
};

function dispatchFrame(data) {
  const handler = FRAME_HANDLERS[data.type];
  if (handler) handler(data.payload);
}

// A dropped socket takes the reply with it: the turn is cancelled and keeps nothing.
// `refused` says this socket was turned away rather than accepted and then lost, which is
// the one case where retrying is bounded instead of open-ended.
function handleSocketClose(refused) {
  clearTimeout(PU.watchdogTimer);
  PU.watchdogTimer = null;
  PU.watchdogToolGrace = false;
  releaseSupersededSocket();
  discardPendingApproval();
  if (PU.isStreaming) {
    removeCurrentAssistantMessage();
    setSendButtonState(false);
    PU.replyLostToDisconnect = true;
  }
  refreshChainLength();
  if (refused) {
    // A page that was never the session's client lost no reply, so the dot goes dark
    // without the notice a lost reply would raise: the refusal on the bar is the story.
    PU.replyLostToDisconnect = false;
    setConnectionState("offline");
    // The page holding the session may be reloading right now, so a refusal is retried a
    // bounded number of times before the reader is told to reload.
    if (PU.busyRefusals <= MAX_BUSY_RETRIES) scheduleReconnect();
    return;
  }
  // Accepted, so whatever refusals came before are spent.
  PU.busyRefusals = 0;
  setConnectionState("offline");
  scheduleReconnect();
}

// Re-draw what the server knows after a reconnect. The transcript is cleared and
// replayed from the store, so a turn that completed while the socket was down appears
// instead of a half-drawn reply.
async function resyncAfterReconnect() {
  // A turn still streaming keeps the bubble it is drawing: the store holds no reply for it
  // yet, so replaying would delete the answer and leave the arriving frames drawing into a
  // message that is no longer on the page.
  if (PU.isStreaming) {
    refreshChainLength();
    return;
  }
  try {
    PU.composer.messagesEl.replaceChildren();
    await loadSession();
    await loadHistory();
    scrollToBottom();
  } catch (_) {}
}

function connectWebSocket() {
  clearTimeout(PU.reconnectTimer);
  PU.reconnectTimer = null;
  // One socket at a time: the server reads a second one from this page as another page.
  if (PU.ws &&
      (PU.ws.readyState === WebSocket.CONNECTING || PU.ws.readyState === WebSocket.OPEN)) {
    return;
  }

  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(
      `${protocol}//${window.location.host}/ws?client=${encodeURIComponent(pageClientId())}`);
  PU.ws = ws;
  let refused = false;

  ws.onopen = () => {
    if (ws !== PU.ws) return;
    PU.reconnectAttempts = 0;
    setConnectionState("online");
    // A reconnect may have missed turns the server processed while the socket was
    // down: the transcript and the chain length are re-read rather than trusted.
    resyncAfterReconnect();
  };

  ws.onmessage = (event) => {
    // A handed-over socket still delivers the turn it was reading until the server closes
    // it, and those frames are not repeated on the replacement.
    if (ws !== PU.ws && ws !== PU.supersededSocket) return;
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (_) {
      return;
    }
    // Anything but a refusal means this page is the session's client.
    if (data.type === "busy") {
      refused = true;
    } else {
      PU.busyRefusals = 0;
    }
    if (data.type === "chunk" || data.type === "tool_start" || data.type === "tool_end" ||
        data.type === "thinking") {
      armWatchdog();
    }
    dispatchFrame(data);
  };

  ws.onclose = () => {
    // A socket the page has already let go of no longer speaks for the connection.
    if (ws !== PU.ws) return;
    PU.ws = null;
    handleSocketClose(refused);
  };
  ws.onerror = () => {};
}
