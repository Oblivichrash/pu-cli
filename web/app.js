// SPDX-License-Identifier: GPL-3.0-only
// The page's entry point. Everything it wires has been moved to a module of its own, so
// this file is only the elements the other modules reach for and the order they start in.
//
// Load order (index.html) is the dependency order: state, the block widgets and the entry
// that picks between them, the frame around a message, the live transcript, the panel,
// the notice strip, history folding, the socket, the session calls, the composer, and
// finally this bootstrap.

"use strict";

PU.composer.messagesEl = document.getElementById("messages");
PU.composer.inputEl = document.getElementById("input");
PU.composer.sendBtn = document.getElementById("send");
PU.composer.agentSelect = document.getElementById("agent-select");
PU.composer.thinkingSelect = document.getElementById("thinking-select");

const messagesEl = PU.composer.messagesEl;
const inputEl = PU.composer.inputEl;

PU.composer.thinkingSelect.addEventListener(
    "change", () => setThinkingLevel(PU.composer.thinkingSelect.value));
PU.composer.sendBtn.addEventListener("click", sendMessage);

inputEl.addEventListener("compositionstart", () => { PU.composer.composing = true; });
inputEl.addEventListener("compositionend", () => { PU.composer.composing = false; });
// While an input method composes, Enter confirms a candidate rather than sending.
inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !PU.composer.composing && !PU.isStreaming) {
    e.preventDefault();
    sendMessage();
  }
});
inputEl.addEventListener("input", () => { inputEl.style.height = "auto"; });

messagesEl.addEventListener("scroll", () => {
  const distance = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight;
  PU.isAtBottom = distance < 10;
});

connectWebSocket();

(async () => {
  await loadAgents();
  await loadSession();
  await loadHistory();
})();
