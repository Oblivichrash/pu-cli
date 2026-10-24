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

// Who a turn is from. The person at the page is "You"; a message that arrived from another
  // pu server is named after the peer that sent it, which the server records on the turn.
function roleLabel(role, fromPeer) {
  if (role !== "user") return "Assistant";
  return fromPeer ? fromPeer : "You";
}

// What a turn cost, as one line: the counts always, a price only where the product reported
  // one. A turn the product did not price says "—" rather than "0", which would read as free.
function usageLine(usage) {
  const parts = [];
  const count = (value) => (Number.isFinite(Number(value)) ? Number(value).toLocaleString() : null);
  const prompt = count(usage.prompt_tokens);
  const completion = count(usage.completion_tokens);
  const cached = count(usage.cached_tokens);
  if (prompt !== null) parts.push(`${prompt} prompt`);
  if (completion !== null) parts.push(`+${completion} completion`);
  if (cached !== null && cached !== "0") parts.push(`${cached} cached`);
  if (usage.cost && Number.isFinite(Number(usage.cost.amount))) {
    parts.push(`${Number(usage.cost.amount)} ${usage.cost.unit || ""}`.trim());
  } else {
    parts.push("— price");
  }
  if (Number.isFinite(Number(usage.rounds)) && Number(usage.rounds) > 1) {
    parts.push(`${Number(usage.rounds)} calls`);
  }
  if (usage.model) parts.push(usage.model);
  return parts.join("  ·  ");
}

// What a turn cost, as one quiet line under the reply. It is short enough to read in full,
  // so there is nothing to fold away.
function usageLineNode(usage) {
  const line = document.createElement("div");
  line.className = "msg-usage";
  line.textContent = usageLine(usage);
  return line;
}

function createMessage(role, blocks, turn, text, fromPeer, usage) {
  const el = document.createElement("div");
  el.className = "msg " + role;

  const label = document.createElement("span");
  label.className = "role";
  label.textContent = roleLabel(role, fromPeer);

  // A turn a peer asked is not this page's to take back, so it carries no edit affordance.
  // /api/rewind refuses it for the same reason.
  if (role === "user" && typeof turn === "number" && turn > 0 && !fromPeer) {
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
  if (role === "assistant" && usage) el.appendChild(usageLineNode(usage));

  // Read the position before the append, because the append itself moves the content
  // the reader is measured against: appending grows the scroll height without firing a
  // scroll event, so a view that only reacted to scrolling would never notice the row
  // it is now one bubble away from.
  const wasAtBottom = isAtBottom();
  PU.composer.messagesEl.appendChild(el);
  if (wasAtBottom) scrollToBottom();
  return el;
}
