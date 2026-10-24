// SPDX-License-Identifier: GPL-3.0-only
// The composer's two jobs: turning what is typed into a run, and turning a second press
// while one is in flight into a cancel.

"use strict";

// Cancelling the turn and stopping a command are different acts, so neither is "Stop".
function setSendButtonState(streaming) {
  const { sendBtn } = PU.composer;
  PU.isStreaming = streaming;
  sendBtn.textContent = streaming ? "Cancel reply" : "Send";
  sendBtn.disabled = false;
  // A setting applies to the next message, so every control is locked while one is in flight.
  for (const control of document.querySelectorAll("[data-setting]")) {
    control.disabled = streaming;
  }
  if (PU.composer.settingsReset) PU.composer.settingsReset.disabled = streaming;
}

function cancelReply() {
  // Cancelling only asks; the partial reply is dropped because the store keeps none.
  if (!sendFrame("cancel")) {
    setSendButtonState(false);
    return;
  }
  PU.stopRequested = true;
  removeCurrentAssistantMessage();
  PU.composer.sendBtn.textContent = "Stopping…";
  PU.composer.sendBtn.disabled = true;
}

function sendMessage() {
  if (PU.isStreaming) {
    cancelReply();
    return;
  }

  const input = PU.composer.inputEl;
  const text = input.value.trim();
  if (!text) return;
  if (!PU.ws || PU.ws.readyState !== WebSocket.OPEN) {
    createSystemMessage("Not connected to the server.");
    return;
  }

  PU.stopRequested = false;
  input.value = "";
  input.style.height = "auto";

  // The store appends this message, so its turn is the next one.
  // What a page sends is its own, so it carries no peer name.
  createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: text }], PU.chainLength + 1, text,
                undefined);
  startAssistantMessage();
  setSendButtonState(true);
  sendFrame("run", { text });
}
