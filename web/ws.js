// SPDX-License-Identifier: GPL-3.0-only
// The page's single socket and the REST calls that go with it: one page owns a session,
// so a refusal is a state to report rather than a connection to keep retrying.

"use strict";

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

// The id names which command and the note is why; the frame already carries both.
function stopRunningCommand(toolId, note) {
  sendFrame("stop_command", { text: note || "", id: toolId });
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

function handleDone(payload) {
  finishAssistantMessage();
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
function handleSocketClose() {
  discardPendingApproval();
  const refused = PU.busyRefusals > 0;
  if (PU.isStreaming) {
    removeCurrentAssistantMessage();
    setSendButtonState(false);
    PU.replyLostToDisconnect = true;
  }
  refreshChainLength();
  if (refused) {
    PU.connState = "offline";
    if (PU.busyRefusals <= MAX_BUSY_RETRIES) scheduleReconnect();
    return;
  }
  setConnectionState("offline");
  scheduleReconnect();
}

// Re-draw what the server knows after a reconnect. The transcript is cleared and
// replayed from the store, so a turn that completed while the socket was down appears
// instead of a half-drawn reply.
async function resyncAfterReconnect() {
  try {
    PU.composer.messagesEl.replaceChildren();
    await loadSession();
    await loadHistory();
    scrollToBottom();
  } catch (_) {}
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  PU.ws = new WebSocket(`${protocol}//${window.location.host}/ws`);

  PU.ws.onopen = () => {
    PU.reconnectAttempts = 0;
    setConnectionState("online");
    // A reconnect may have missed turns the server processed while the socket was
    // down: the transcript and the chain length are re-read rather than trusted.
    resyncAfterReconnect();
  };

  PU.ws.onmessage = (event) => {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (_) {
      return;
    }
    // Anything but a refusal means this page is the session's client.
    if (data.type !== "busy") PU.busyRefusals = 0;
    dispatchFrame(data);
  };

  PU.ws.onclose = handleSocketClose;
  PU.ws.onerror = () => {};
}
