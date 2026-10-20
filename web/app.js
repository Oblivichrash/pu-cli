// SPDX-License-Identifier: GPL-3.0-only
// The page's entry point: the elements the other modules reach for, and the order they
// start in.

"use strict";

PU.composer.messagesEl = document.getElementById("messages");
PU.composer.inputEl = document.getElementById("input");
PU.composer.sendBtn = document.getElementById("send");
PU.composer.agentSelect = document.getElementById("agent-select");
PU.composer.settingsRows = document.getElementById("settings-rows");
PU.composer.settingsReset = document.getElementById("settings-reset");

PU.composer.settingsReset.addEventListener("click", resetSettings);
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

// The transcript has one loader: the socket's first open runs `resyncAfterReconnect()`,
// which reads the store once the session is attached. Reading it here as well made the two
// runs append the same turns to the same list, which is the turn drawn twice on every load.
connectWebSocket();

(async () => {
  await loadAgents();
  await loadSession();
})();

// The board is read on its own clock: it changes when a peer moves a job, which is not
// something this page's socket reports.
startBoardPolling();
