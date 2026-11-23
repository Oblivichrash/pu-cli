// SPDX-License-Identifier: GPL-3.0-only
// The composer's two jobs: turning what is typed into a run, and turning a second press
// while one is in flight into a stop.

"use strict";

// One button, one mark. What the frame ends is a turn and whatever it has running, and a
// reader who presses it means both: splitting the mark would offer a choice the protocol does
// not make.
//
// The mark says what the next press does rather than what is happening: an arrow while a turn is
// the thing that press would start, a square while one is running and that press would end it.
// The words are in the title and the label, where a reader who needs them finds them -- the field
// is small and a word in it would take the room the sentence needs.
function renderSendButton() {
  const { sendBtn } = PU.composer;
  // The arrow is two paths -- a head and a shaft -- because that is how the icon set draws it, so
  // both are toggled rather than the first one found.
  const sendPaths = sendBtn.querySelectorAll(".icon-send");
  const stopPath = sendBtn.querySelector(".icon-stop");
  // A field being edited is still a field to fill, so the edit keeps the arrow too: what the press
  // does is replace a message, which is a send like any other from where the reader is standing.
  const stopping = PU.stopRequested;
  const streaming = PU.isStreaming;
  // `hidden` is a reflected boolean property on HTML elements, but on an SVG element it is only an
  // attribute the UA sheet happens to style -- assigning `false` leaves it in the DOM. The attribute
  // is toggled here instead, which is the form that works in both directions.
  for (const path of sendPaths) path.toggleAttribute("hidden", streaming);
  stopPath.toggleAttribute("hidden", !streaming);
  // "Stopping…" is the window between asking and the server's `done`: without it a press that is
  // still on its way looks like one that did nothing, so the square dims and the press is refused.
  sendBtn.disabled = stopping;
  sendBtn.setAttribute("aria-label", streaming ? (stopping ? "Stopping" : "Stop") : "Send");
  sendBtn.title = streaming ? (stopping ? "Stopping…" : "Stop the reply") : "Send (Enter)";
}

// The field grows to its own text and no further. Both the typed message and the edited one land
// in the same box, so the box's height is one fact and one function settles it.
function resizeComposerInput() {
  const input = PU.composer.inputEl;
  if (!input) return;
  input.style.height = "auto";
  input.style.height = input.scrollHeight + "px";
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
//
// `forcedTurn` is the number the caller already knows the turn has. An edit that stepped the store
// back needs the bubble it draws to carry the number the store gave it, and that number is not
// `chainLength + 1` until `refreshChainLength` has come back -- reading it here would race the
// refresh and number the replacement turn wrongly.
function sendText(text, existing, forcedTurn) {
  if (!text) return false;
  if (!PU.ws || PU.ws.readyState !== WebSocket.OPEN) {
    createSystemMessage("Not connected to the server.");
    return false;
  }
  PU.stopRequested = false;
  // The turn is the stored message count plus one, which is what /api/rewind takes. A message
  // being re-sent after a rewind keeps the turn it already carries, because the rewind left it
  // the last thing in the chain and the browser now agrees with the store.
  const turn = forcedTurn || (existing ? Number(existing.dataset.turn) || PU.chainLength + 1
                                        : PU.chainLength + 1);
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
  // An open edit is what this button ends. It is checked before the field is read, because the
  // field holds the sentence being edited rather than the one being typed.
  if (PU.editing) {
    sendEdit();
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
