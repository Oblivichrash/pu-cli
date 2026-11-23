// SPDX-License-Identifier: GPL-3.0-only
// The configuration dialog: everything the reader writes rather than reads. What the conversation
// has spent is the opposite kind of thing and lives in the status bar.

"use strict";

function configDialog() {
  return document.getElementById("config-dialog");
}

function configOpen() {
  const dialog = configDialog();
  if (!dialog || dialog.open) return;
  // A turn in flight is the one state the dialog cannot serve: every control in it writes to the
  // configuration the running turn already read. The button says so rather than opening onto a grid
  // of greyed-out boxes.
  if (PU.isStreaming) {
    createSystemMessage("A reply is still arriving. Wait for it to finish, or stop it, before changing the configuration.");
    return;
  }
  // The model menu is filled by asking the gateway and the answer is not cached, so opening asks
  // now: the list belongs to the gateway in force rather than to whichever one was looked at last.
  loadModelsFor(PU.composer.gatewaySelect.value);
  dialog.showModal();
}

// The form is `method="dialog"`, so the close button ends the dialog without a handler. Escape is
// handled here rather than left to the browser's own `cancel`, which arrives on the dialog only when
// the platform chooses to deliver it; a page that binds Escape for an approval needs the two never
// to both see the key.
function wireConfigDialog() {
  const dialog = configDialog();
  const opener = document.getElementById("config-open");
  if (opener) opener.addEventListener("click", configOpen);
  if (!dialog) return;
  dialog.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || e.target.tagName === "SELECT") return;
  // A dropdown closes on Escape before the dialog hears it, so the key does not leak out of an open
  // menu and shut the sheet behind it.
  e.preventDefault();
    e.stopPropagation();
    dialog.close();
  });
  // A click on the backdrop lands on the dialog element itself, since the scroll box inside it
  // fills the box. Closing on that click is what a reader expects of a modal.
  dialog.addEventListener("click", (e) => {
    if (e.target === dialog) dialog.close();
  });
}

wireConfigDialog();
