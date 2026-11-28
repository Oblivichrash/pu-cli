// SPDX-License-Identifier: GPL-3.0-only
// The page's single socket and the REST calls that go with it: one page owns a session, so the
// page keeps exactly one socket and reports a refusal instead of taking the session from the page
// that holds it.
//
// A reconnect hands over the state of the turn in flight and of the last finished one, as frames
// of their own: a chunk appends to a turn, a snapshot replaces it. Nothing is held back while the
// page catches up, because a turn drawn twice ends up on the page once.

"use strict";

// A turn that says nothing for this long is treated as a dead socket. A command can run
// for minutes without a frame, so the wait is longer while one is running.
const WATCHDOG_STREAM_MS = 60000;
const WATCHDOG_TOOL_MS = 300000;

// The tab's name for itself. sessionStorage keeps it across a reload and gives every other tab its
// own, so the server can tell this page's reconnects from a second page.
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
  // A subtask is a tool that runs a conversation elsewhere: it holds the socket open as long as a
  // command would, so it earns the same longer wait.
  return PU.currentAssistantBlocks.some(
      (block) => (block.type === BLOCK_TYPES.TOOL_CALL || block.type === BLOCK_TYPES.SUBTASK) &&
          block.status === "running");
}

// Drop the watchdog without reopening anything: the timer is not pending any more, and a command
// that was given its longer wait starts over with the next turn.
function clearWatchdog() {
  clearTimeout(PU.watchdogTimer);
  PU.watchdogTimer = null;
  PU.watchdogToolGrace = false;
}

// A turn that stops speaking without a close is indistinguishable from a hung socket, so the socket
// is reopened. The timer is re-armed on every progress frame.
function armWatchdog() {
  // Progress arrived, so a running command is no longer silent.
  clearWatchdog();
  PU.watchdogTimer = setTimeout(onWatchdogElapsed, WATCHDOG_STREAM_MS);
}

function onWatchdogElapsed() {
  PU.watchdogTimer = null;
  if (!PU.isStreaming) return;
  // A command that is still running says nothing until it ends, so the silence is not a hang. It is
  // given the longer wait once rather than indefinitely: a tool that died without its `tool_end`
  // must still lead to a reopened socket.
  if (toolIsRunning() && !PU.watchdogToolGrace) {
    PU.watchdogToolGrace = true;
    PU.watchdogTimer = setTimeout(onWatchdogElapsed, WATCHDOG_TOOL_MS);
    return;
  }
  reopenSilentSocket();
}

// The socket being given up on is handed over, not closed. Its close would reach the server as this
// page going away and cancel the turn still being written; instead the server closes it once the
// replacement has taken the session. Until then its frames keep being drawn, since they are this
// turn's, and its close is ignored: that close is the handover rather than the page leaving.
function reopenSilentSocket() {
  PU.watchdogTimer = null;
  PU.watchdogToolGrace = false;
  if (!PU.isStreaming) return;
  // Only one socket can be waiting to be closed by the server: an older one has had its turn
  // handed on already.
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

// Let go of the handed-over socket once its turn is over. Closing it while the turn runs would be
// read as the page leaving; after `done` there is no turn in flight, which is why the release waits
// for the end of the turn.
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
  // name it instead of leaving the box empty. A `done` with no bubble at all belongs to a
  // turn whose frames arrived while the page was loading the transcript, and whose reply
  // the store already shows; naming that one would invent a message.
  if (PU.currentAssistantEl) {
    if (PU.currentAssistantBlocks.length === 0) {
      removeCurrentAssistantMessage();
      createSystemMessage("The reply came back empty. Try rephrasing, or check the backend.");
    } else {
      finishAssistantMessage();
    }
  }
  clearWatchdog();
  // The turn is over, so the socket it was handed over from has nothing left to deliver.
  releaseSupersededSocket();
  setSendButtonState(false);
  refreshChainLength();
  if (PU.stopRequested) {
    createSystemMessage("Stopped.");
    PU.stopRequested = false;
  }
  if (payload && payload.model) setBackendLabel(PU.gatewayShown, payload.model);
}

function handleError(payload) {
  const errMsg = payload.text || "Unknown error";
  if (PU.currentAssistantEl) {
    discardAssistantRender();
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
  turn_snapshot: handleTurnSnapshot,
  done: handleDone,
  error: handleError,
  notice: handleNotice,
  usage: showUsage,
  busy: handleBusy,
};

function dispatchFrame(data) {
  const handler = FRAME_HANDLERS[data.type];
  if (handler) handler(data.payload);
}

// A dropped socket takes the reply with it: the turn is cancelled and keeps nothing. `refused` says
// this socket was turned away rather than accepted and then lost, which is the one case where
// retrying is bounded instead of open-ended.
function handleSocketClose(refused) {
  clearWatchdog();
  releaseSupersededSocket();
  discardPendingApproval();
  if (PU.isStreaming) {
    removeCurrentAssistantMessage();
    setSendButtonState(false);
    PU.replyLostToDisconnect = true;
  }
  refreshChainLength();
  if (refused) {
    // A page that was never the session's client lost no reply, so the dot goes dark without the
    // notice a lost reply would raise: the refusal on the bar is the story.
    PU.replyLostToDisconnect = false;
    setConnectionState("offline");
    // The page holding the session may be reloading, so a refusal is retried a bounded number of
    // times before the reader is told to reload.
    if (PU.busyRefusals <= MAX_BUSY_RETRIES) scheduleReconnect();
    return;
  }
  // Accepted, so whatever refusals came before are spent.
  PU.busyRefusals = 0;
  setConnectionState("offline");
  scheduleReconnect();
}

// The chain length is the one thing a snapshot does not carry, so it is always re-read.
//
// The store itself is read only into an empty transcript. A first open arrives with nothing
// drawn and nothing handed over but the last turn or two, so the chain has to be fetched. A
// reconnect arrives with the chain already on the page: the server hands over the turn it missed
// as a snapshot, which `applyTurn` replaces in place, and reading the store again would append
// the turns the page already holds. The two are told apart by whether anything is drawn, not by
// whether a turn is running: a page whose turn was in flight when the socket dropped still has
// that turn's bubble, and so still does not need the store.
function resyncAfterReconnect() {
  refreshChainLength();
  if (PU.composer.messagesEl.childElementCount === 0) loadHistory().then(scrollToBottom);
}

function connectWebSocket() {
  clearTimeout(PU.reconnectTimer);
  PU.reconnectTimer = null;
  // One socket at a time: the server reads a second one from this page as another page.
  if (PU.ws &&
      (PU.ws.readyState === WebSocket.CONNECTING || PU.ws.readyState === WebSocket.OPEN)) {
    return;
  }

  // The socket follows the server the page is attached to, which is the one that served it until
  // the reader switches. `PU.self.origin` is always set, so there is no fallback to get wrong.
  const url = new URL(PU.self.origin);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  url.pathname = "/ws";
  url.search = `?client=${encodeURIComponent(pageClientId())}`;
  const ws = new WebSocket(url.toString());
  PU.ws = ws;
  let refused = false;

  ws.onopen = () => {
    if (ws !== PU.ws) return;
    PU.reconnectAttempts = 0;
    setConnectionState("online");
    // A reconnect may have missed turns processed while the socket was down, so the transcript and
    // the chain length are re-read rather than trusted. Frames arriving before that read finishes
    // belong to a turn still running and are held rather than drawn into boxes about to go.
    resyncAfterReconnect();
  };

  ws.onmessage = (event) => {
    // A handed-over socket still delivers the turn it was reading until the server closes it, and
    // those frames are not repeated on the replacement.
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
        data.type === "thinking" || data.type === "turn_snapshot") {
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
