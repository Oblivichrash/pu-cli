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
  isStreaming: false,
  stopRequested: false,
  reconnectAttempts: 0,
  reconnectTimer: null,
  connState: "connecting",
  replyLostToDisconnect: false,
  busyRefusals: 0,
  watchdogTimer: null,

  // The next message sent is turn chainLength + 1, which is the number /api/rewind expects.
  chainLength: 0,
  currentAssistantBlocks: [],
  currentAssistantEl: null,
  backendTypeShown: "",

  composer: {
    composing: false,
    inputEl: null,
    sendBtn: null,
    messagesEl: null,
    agentSelect: null,
    thinkingSelect: null,
    agentChangeHandler: null,
  },
};

function setConnectionState(state) {
  if (state === PU.connState) return;
  const dot = document.getElementById("conn-dot");
  if (dot) {
    dot.className = "conn-dot " + state;
    dot.title = state === "online" ? "Connected"
      : (state === "offline" ? "Disconnected" : "Reconnecting…");
  }
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
