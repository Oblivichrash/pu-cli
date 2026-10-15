// SPDX-License-Identifier: GPL-3.0-only
// The page's entry point: the elements the other modules reach for, and the order they
// start in.

"use strict";

PU.composer.messagesEl = document.getElementById("messages");
PU.composer.inputEl = document.getElementById("input");
PU.composer.sendBtn = document.getElementById("send");
PU.composer.agentSelect = document.getElementById("agent-select");
PU.composer.thinkingSelect = document.getElementById("thinking-select");

PU.composer.thinkingSelect.addEventListener(
    "change", () => setThinkingLevel(PU.composer.thinkingSelect.value));
PU.composer.sendBtn.addEventListener("click", sendMessage);

PU.composer.inputEl.addEventListener("compositionstart", () => { PU.composer.composing = true; });
PU.composer.inputEl.addEventListener("compositionend", () => { PU.composer.composing = false; });
// While an input method composes, Enter confirms a candidate rather than sending.
PU.composer.inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !PU.composer.composing && !PU.isStreaming) {
    e.preventDefault();
    sendMessage();
  }
});
PU.composer.inputEl.addEventListener("input", () => { PU.composer.inputEl.style.height = "auto"; });

const messagesEl = PU.composer.messagesEl;
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
