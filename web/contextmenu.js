// SPDX-License-Identifier: GPL-3.0-only
// What may be done with a message, offered where a reader already points at it.

"use strict";

// A message at rest is its words and, on a reply, what the turn cost. Nothing else: a row of
// marks under every turn is furniture the eye reading the conversation has to step over, and on
// a page of twenty turns it is twenty rows of it. The controls moved into the bar the editor
// opens, and then the reply's own bar closed as soon as the pointer left -- a bar that appears on
// a click and vanishes on the next is a thing a reader has to learn before they can use it.
//
// The right button is the gesture that says "what can be done with this" without being offered:
// a reader who wants the words elsewhere already right-clicks to take them. So the menu is the
// only way in, it opens on the message itself rather than in a bar below it, and a message at
// rest still draws nothing.
//
// Three kinds of right-click are not ours and must be left alone:
//   * a text field's, which in the window this page runs in is the only way to paste;
//   * a selection's, made by hand inside a message, where the system's own copy is the answer;
//   * a tool card's, which holds command output a reader may want the terminal to copy.
// Taking any of those would be a feature removed in the name of one added.

const MENU_ID = "msg-context-menu";

// The menu is one shared element rather than one per message: there is one pointer, so there is
// one menu, and a node that outlives the messages it acts on cannot outlive the element it acts
// on. It is built empty and filled on each open, because what a message offers depends on the
// message -- a reply can be copied, a turn of yours can also be edited.
function contextMenuNode() {
  let menu = document.getElementById(MENU_ID);
  if (menu) return menu;
  menu = document.createElement("div");
  menu.id = MENU_ID;
  menu.className = "context-menu";
  menu.setAttribute("role", "menu");
  menu.hidden = true;
  // A press inside the menu is not a press on the page behind it, and the menu must not be
  // closed by the same listener that closes it for presses elsewhere.
  menu.addEventListener("contextmenu", (event) => event.preventDefault());
  document.body.appendChild(menu);
  return menu;
}

function contextMenuOpen() {
  const menu = document.getElementById(MENU_ID);
  return menu && !menu.hidden ? menu : null;
}

function closeContextMenu() {
  const menu = document.getElementById(MENU_ID);
  if (!menu || menu.hidden) return;
  menu.hidden = true;
  menu.replaceChildren();
  menu.removeAttribute("data-for");
}

// A message's own right-click, or one inside it. Reading the closest message rather than the
// target itself is what lets a reader right-click a word inside a reply and mean the reply.
function messageUnderEvent(event) {
  const target = event.target;
  if (!target || !target.closest) return null;
  return target.closest(".msg");
}

// The three refusals at the top of this file, in the order they can be decided: the field is
// about pasting, a selection is about the words the reader chose, and a tool card is another
// application's output that this menu has no opinion about.
function mayOfferControls(event, el) {
  if (event.target.closest("textarea, input, select")) return false;
  if (event.target.closest(".block-tool, .block-subtask, .block-thinking")) return false;
  const selection = window.getSelection();
  if (selection && selection.toString().trim()) return false;
  return Boolean(el);
}

// Turns a message into menu items. The list is short and every entry is a whole sentence about
// what it does, because a menu is read once by a reader who is already looking for one line in
// it, and an icon with a tooltip is a thing that has to be pointed at to be understood.
function menuItems(el) {
  const items = [];
  const isUser = el.classList.contains("user");
  const turn = Number(el.dataset.turn);

  items.push({
    label: "Copy message",
    run: () => {
      // The stored blocks, not the DOM: a reply taken from the rendered page pastes with its
      // markdown flattened back to plain text, which is a different sentence from the one the
      // model wrote.
      const text = messageText(blocksOf(el));
      if (!text) {
        createSystemMessage("There is no text in this message to copy.");
        return;
      }
      if (copyMessage(text)) createSystemMessage("Copied to the clipboard.");
    },
  });

  // A turn of yours is a thing you may have meant differently. A reply has nothing to change and
  // no turn to take back, so it is offered nothing here rather than something disabled: a greyed
  // row says "this exists and you may not have it", which is not what a reply is.
  if (isUser && Number.isFinite(turn) && turn > 0) {
    items.push({
      label: "Edit message",
      run: () => beginEdit(el, null, true),
    });
  }

  return items;
}

function openContextMenu(el, event) {
  const menu = contextMenuNode();
  closeContextMenu();
  menu.replaceChildren();

  const buttons = [];
  menuItems(el).forEach((item) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "context-item";
    button.setAttribute("role", "menuitem");
    button.textContent = item.label;
    button.addEventListener("click", () => {
      closeContextMenu();
      item.run();
    });
    menu.appendChild(button);
    buttons.push(button);
  });

  menu.dataset.for = el.dataset.turn || "";
  menu.hidden = false;
  placeContextMenu(menu, event.clientX, event.clientY);
  // Focus is moved into the menu rather than left on the page: the keys below act on whatever
  // holds focus, and a menu a reader cannot walk with the keyboard is a menu half the readers
  // cannot use at all.
  if (buttons.length) buttons[0].focus();
}

// A menu laid out at the pointer's coordinates but measured after it is in the flow: a menu that
// runs off the right or the bottom is unreadable past the edge of the window, so the position is
// corrected by the size the menu actually came out at rather than by an estimate of it.
function placeContextMenu(menu, x, y) {
  menu.style.left = "0px";
  menu.style.top = "0px";
  const rect = menu.getBoundingClientRect();
  const margin = 6;
  const left = Math.min(x, window.innerWidth - rect.width - margin);
  const top = Math.min(y, window.innerHeight - rect.height - margin);
  menu.style.left = Math.max(margin, left) + "px";
  menu.style.top = Math.max(margin, top) + "px";
}

function moveContextMenuFocus(step) {
  const menu = contextMenuOpen();
  if (!menu) return;
  const items = [...menu.querySelectorAll(".context-item")];
  const at = items.indexOf(document.activeElement);
  const next = (at + step + items.length) % items.length;
  items[next].focus();
}

document.addEventListener("contextmenu", (event) => {
  const el = messageUnderEvent(event);
  if (!mayOfferControls(event, el)) {
    // Not ours: the menu the window would have drawn is the one the reader asked for. Saying
    // nothing leaves it to be drawn, which is the whole of what leaving it alone means.
    closeContextMenu();
    return;
  }
  event.preventDefault();
  openContextMenu(el, event);
});

// Anything that is not the menu itself is a reader moving on. The capture phase is what makes
// this true of presses whose handlers stop propagation: a menu closed by a press must close for
// every press, or it stays on screen over a message that is no longer the one being acted on.
document.addEventListener(
  "click",
  (event) => {
    if (event.target.closest && event.target.closest("#" + MENU_ID)) return;
    closeContextMenu();
  },
  true
);

document.addEventListener("keydown", (event) => {
  if (!contextMenuOpen()) return;
  if (event.key === "Escape") {
    event.preventDefault();
    closeContextMenu();
    return;
  }
  if (event.key === "ArrowDown") {
    event.preventDefault();
    moveContextMenuFocus(1);
    return;
  }
  if (event.key === "ArrowUp") {
    event.preventDefault();
    moveContextMenuFocus(-1);
    return;
  }
  if (event.key === "Tab") {
    // A menu is a small set of rows rather than a region of the page: tabbing out of it is
    // tabbing past the message, so it closes behind the reader as they leave.
    closeContextMenu();
  }
});

// Scrolling under an open menu leaves it pointing at where the message used to be. The message
// it names is gone from under the pointer, so the menu goes with it.
window.addEventListener("scroll", () => closeContextMenu(), true);
window.addEventListener("resize", () => closeContextMenu());
window.addEventListener("blur", () => closeContextMenu());
