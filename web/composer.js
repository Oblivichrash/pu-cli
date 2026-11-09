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

// One place that starts a turn. The composer and an edited message both come here, so a
// re-sent turn is drawn and announced exactly like a typed one: two paths that each assembled
// the same bubble would drift, and the drift would show as a message that looked different
// depending on which box it came from.
//
// `existing` is the bubble a re-sent turn is already drawn in. An edited message keeps its place
// in the transcript, so a second copy of it would be a duplicate of the same sentence; the caller
// passes the bubble it is using and nothing is created.
function sendText(text, existing) {
  if (!text) return false;
  if (!PU.ws || PU.ws.readyState !== WebSocket.OPEN) {
    createSystemMessage("Not connected to the server.");
    return false;
  }
  PU.stopRequested = false;
  // The turn is the stored message count plus one, which is what /api/rewind takes. A message
  // being re-sent after a rewind keeps the turn it already carries, because the rewind left it
  // the last thing in the chain and the browser now agrees with the store.
  const turn = existing ? Number(existing.dataset.turn) || PU.chainLength + 1
                        : PU.chainLength + 1;
  const el = existing ||
      createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: text }], turn, text);
  el.dataset.turn = String(turn);
  el.dataset.text = text;
  startAssistantMessage();
  setSendButtonState(true);
  sendFrame("run", { text });
  return true;
}

function sendMessage() {
  if (PU.isStreaming) {
    stopReply();
    return;
  }

  const input = PU.composer.inputEl;
  const text = input.value.trim();
  if (!text) return;
  if (PU.ws && PU.ws.readyState === WebSocket.OPEN) {
    input.value = "";
    input.style.height = "auto";
  }
  sendText(text);
}
