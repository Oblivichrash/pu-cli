// SPDX-License-Identifier: GPL-3.0-only
// The page's entry point: the elements the other modules reach for, and the order they
// start in.

"use strict";

PU.announce = showNotice;

PU.composer.messagesEl = document.getElementById("messages");
PU.composer.inputEl = document.getElementById("input");
PU.composer.sendBtn = document.getElementById("send");
PU.composer.gatewaySelect = document.getElementById("kb-type");
PU.composer.modelSelect = document.getElementById("kb-model");
PU.composer.modelText = document.getElementById("kb-model-text");
PU.composer.settingsRows = document.getElementById("settings-rows");
PU.composer.settingsReset = document.getElementById("settings-reset");

PU.composer.settingsReset.addEventListener("click", resetSettings);
// Opening the menu asks what the gateway serves, rather than having been told once at start:
// the catalogue belongs to the gateway, and only the one being looked at is worth a request.
// The dialog asks as well when it opens, since a reader who changes the gateway there must find
// the new one's models already listed in the menu beside it.
PU.composer.modelSelect.addEventListener("pointerdown", () => loadModelsFor(PU.composer.gatewaySelect.value));
PU.composer.gatewaySelect.addEventListener("change", () => {
  // The model menu belongs to the gateway: switching empties it and asks the new one, so the
  // names offered are the ones that gateway serves.
  const gateway = PU.composer.gatewaySelect.value;
  loadModelsFor(gateway);
  chooseBackend(gateway, null);
});
PU.composer.modelSelect.addEventListener("change", () => chooseBackend(PU.composer.gatewaySelect.value, PU.composer.modelSelect.value));
// A gateway that lists no models -- a key that is not set, a host that is down -- leaves the
// model as typed text. The text is committed on Enter, not on every key, so a half-typed name
// is never sent.
PU.composer.modelText.addEventListener("keydown", (e) => {
  if (e.key !== "Enter") return;
  e.preventDefault();
  chooseBackend(PU.composer.gatewaySelect.value, PU.composer.modelText.value.trim());
});
PU.composer.sendBtn.addEventListener("click", sendMessage);

PU.composer.inputEl.addEventListener("compositionstart", () => { PU.composer.composing = true; });
PU.composer.inputEl.addEventListener("compositionend", () => { PU.composer.composing = false; });
// While an input method composes, Enter confirms a candidate rather than sending.
PU.composer.inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !PU.composer.composing && !PU.isStreaming) {
    e.preventDefault();
    sendMessage();
  } else if (e.key === "Escape" && PU.editing) {
    // Escape is the way out of an edit. It is bound here rather than on a Cancel button because
    // the button is gone: an edit is cancelled by leaving it, and the field is where the reader
    // already is.
    e.preventDefault();
    cancelEdit(true);
  }
});
PU.composer.inputEl.addEventListener("input", resizeComposerInput);

// The transcript has one loader: the socket's first open runs `resyncAfterReconnect()`,
// which reads the store once the session is attached. Reading it here as well made the two
// runs append the same turns to the same list, which is the turn drawn twice on every load.
//
// The page's own identity is already set: `state.js` reads it from the address that served this
// document, so nothing has to be fetched before the socket opens. The label a reader sees arrives
// with `/api/session`, and the list of other servers arrives on its own clock.
connectWebSocket();

(async () => {
  await loadBackends();
  await loadSession();
})();

// The servers list is read on its own clock: it changes when another server starts, which is
// not something this page's socket reports.
startServerPolling();
