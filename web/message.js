// SPDX-License-Identifier: GPL-3.0-only
// A message is a role, a container of blocks, and the edit affordance a user turn needs.

"use strict";

function renderBlocks(blocks, container) {
  container.replaceChildren();
  for (const block of blocks) {
    const el = createBlockNode(block);
    if (el) container.appendChild(el);
  }
}

function createMessage(role, blocks, turn, text) {
  const el = document.createElement("div");
  el.className = "msg " + role;

  const label = document.createElement("span");
  label.className = "role";
  label.textContent = role === "user" ? "You" : "Assistant";

  if (role === "user" && typeof turn === "number" && turn > 0) {
    const head = document.createElement("div");
    head.className = "msg-head";
    head.appendChild(label);

    const editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.className = "edit-btn";
    editBtn.textContent = "edit";
    editBtn.onclick = () => rewindToTurn(turn, text || "");
    head.appendChild(editBtn);

    el.appendChild(head);
  } else {
    el.appendChild(label);
  }

  const container = document.createElement("div");
  container.className = "blocks-container";
  el.appendChild(container);

  if (blocks) renderBlocks(blocks, container);

  PU.composer.messagesEl.appendChild(el);
  return el;
}
