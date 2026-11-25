// SPDX-License-Identifier: GPL-3.0-only
// The page's entry point: the elements the other modules reach for, and the order they
// start in.

"use strict";

PU.announce = showNotice;

PU.composer.messagesEl = document.getElementById("messages");
PU.composer.inputEl = document.getElementById("input");
PU.composer.sendBtn = document.getElementById("send");
PU.composer.gatewaySelect = document.getElementById("gw-type");
PU.composer.hostInput = document.getElementById("gw-host");
PU.composer.settingsRows = document.getElementById("settings-rows");
PU.composer.settingsReset = document.getElementById("settings-reset");

PU.composer.settingsReset.addEventListener("click", resetSettings);
// Nothing in this form writes until Save. The controls hold an intention, not a change: a reader
// who tries three gateways looking for the one they meant should not have switched the backend
// three times, and an address half typed is not an address to send. A gateway switch is therefore
// only drawn here; the model, which is the other half of the same decision, is chosen from the bar
// once the gateway is in force.
//
// The address is drawn along with it, because it is a fact about the gateway and not about the
// form: leaving the previous gateway's host in the box would show an address for another vendor's
// endpoint, and saving would post this gateway's name to it. The box takes the new gateway's own
// default, which is the address the server would dial if the file named none, and the reader may
// then edit it -- a proxy, a local mirror, a gateway served on another port. Without this the box
// would be filled once at open and go on showing the address of a gateway they had left.
const followGateway = () => {
  const gateway = PU.composer.gatewaySelect ? PU.composer.gatewaySelect.value : "";
  const hosts = PU.gatewayHosts || {};
  if (!gateway) return;
  if (PU.composer.hostInput) PU.composer.hostInput.value = hosts[gateway] || "";
  loadModelsFor(gateway);
};
PU.composer.gatewaySelect.addEventListener("change", followGateway);
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
