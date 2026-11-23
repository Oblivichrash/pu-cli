// SPDX-License-Identifier: GPL-3.0-only
// The configuration dialog: everything the reader writes rather than reads. What the conversation
// has spent is the opposite kind of thing and lives in the status bar.

"use strict";

function configDialog() {
  return document.getElementById("config-dialog");
}

function configOpen(selector) {
  const dialog = configDialog();
  if (!dialog || dialog.open) return;
  // A turn in flight is the one state the dialog cannot serve: every control in it writes to the
  // configuration the running turn already read. The button says so rather than opening onto a grid
  // of greyed-out boxes.
  if (PU.isStreaming) {
    createSystemMessage("A reply is still arriving. Wait for it to finish, or stop it, before changing the configuration.");
    return;
  }
  dialog.showModal();
  // A press in the status bar names the fact it wants to change, so the sheet opens with that
  // field in hand rather than at its top. The brief ring is what answers "where did I land" -- a
  // sheet this tall opens onto a wall of rows, and a dialog that only scrolls to a field still
  // leaves the reader hunting for it. The ring is removed after it has been seen, so the field is
  // not left looking permanently focused.
  //
  // The move waits a frame. `showModal` puts the sheet up and only then settles focus on its first
  // control, so a `focus` called on the same tick is undone a moment later and the reader lands on
  // the gateway whatever they pressed. One frame is enough for the dialog to have its focus.
  if (selector) {
    requestAnimationFrame(() => {
      const field = dialog.querySelector(selector);
      if (!field) return;
      field.focus({ preventScroll: false });
      const holder = field.closest(".field") || field;
      // The ring is per open, not per field: opening the sheet from one press and then another
      // should never leave two rows lit at once, and the previous timeout must not strip the ring
      // from a row this press just lit.
      for (const lit of dialog.querySelectorAll(".field-flash")) {
        if (lit !== holder) lit.classList.remove("field-flash");
      }
      holder.classList.add("field-flash");
      clearTimeout(holder.flashTimer);
      holder.flashTimer = setTimeout(() => holder.classList.remove("field-flash"), 1200);
    });
  }
}

// The form is `method="dialog"`, so the close button ends the dialog without a handler. Escape is
// handled here rather than left to the browser's own `cancel`, which arrives on the dialog only when
// the platform chooses to deliver it; a page that binds Escape for an approval needs the two never
// to both see the key.
function wireConfigDialog() {
  const dialog = configDialog();
  const opener = document.getElementById("config-open");
  if (opener) opener.addEventListener("click", () => configOpen());
  if (!dialog) return;
  // The reasoning level is the one fact in the bar that still lives in this sheet -- the gateway,
  // its address and the model moved to their own form, because they are where the conversation
  // goes rather than how it answers. It is reached in two steps: the item in the bar presses the
  // row, and the row's control takes focus.
  //
  // The row is named by a selector rather than an id because it is built from the server's own
  // list of settings: it exists only if the server offers it, and it is the control inside the
  // row -- not the row -- that takes focus.
  const jumps = [
    ["status-thinking", '#settings-rows [data-setting="thinking"]'],
  ];
  for (const [itemId, selector] of jumps) {
    const item = document.getElementById(itemId);
    if (!item) continue;
    item.addEventListener("click", () => configOpen(selector));
    // These are spans given a role, so the browser does not press them for the reader. Enter and
    // Space are what a button answers to, and answering both is what keeps the bar's shortcut from
    // being a mouse-only way in.
    item.addEventListener("keydown", (e) => {
      if (e.key !== "Enter" && e.key !== " " && e.key !== "Spacebar") return;
      e.preventDefault();
      configOpen(selector);
    });
  }
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
