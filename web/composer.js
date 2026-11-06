// SPDX-License-Identifier: GPL-3.0-only
// The composer's two jobs: turning what is typed into a run, and turning a second press
// while one is in flight into a stop.

"use strict";

// One button, one word. What the frame ends is a turn and whatever it has running, and a
// reader who presses it means both: splitting the label would offer a choice the protocol
// does not make.
function renderSendButton() {
  const { sendBtn } = PU.composer;
  if (!PU.isStreaming) {
    sendBtn.textContent = "Send";
    sendBtn.disabled = false;
    return;
  }
  // "Stopping…" is the window between asking and the server's `done`: without it a press
  // that is still on its way looks like one that did nothing.
  sendBtn.textContent = PU.stopRequested ? "Stopping…" : "Stop";
  sendBtn.disabled = PU.stopRequested;
}

function setSendButtonState(streaming) {
  PU.isStreaming = streaming;
  // A setting applies to the next message, so every control is locked while one is in flight.
  for (const control of document.querySelectorAll("[data-setting]")) {
    control.disabled = streaming;
  }
  if (PU.composer.settingsReset) PU.composer.settingsReset.disabled = streaming;
  renderSendButton();
}

function stopReply() {
  // Stopping only asks; the partial reply is dropped because the store keeps none.
  if (!sendFrame("cancel")) {
    setSendButtonState(false);
    return;
  }
  PU.stopRequested = true;
  removeCurrentAssistantMessage();
  renderSendButton();
}

function sendMessage() {
  if (PU.isStreaming) {
    stopReply();
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
