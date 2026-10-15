// SPDX-License-Identifier: GPL-3.0-only
// The composer's two jobs: turning what is typed into a run, and turning a second press
// while one is in flight into a cancel. Both are one act from the reader's side, so the
// button says which it will be rather than naming itself once.

"use strict";

// Cancelling the turn and stopping a command are different acts, so neither is "Stop".
function setSendButtonState(streaming) {
  const { sendBtn, thinkingSelect } = PU.composer;
  PU.isStreaming = streaming;
  sendBtn.textContent = streaming ? "Cancel reply" : "Send";
  sendBtn.disabled = false;
  // The level applies to the next message, so it is locked while one is in flight.
  thinkingSelect.disabled = streaming;
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
  createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: text }], PU.chainLength + 1, text);
  startAssistantMessage();
  setSendButtonState(true);
  sendFrame("run", { text });
}
