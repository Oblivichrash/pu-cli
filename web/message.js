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
  // No model name here. A turn's cost says what this reply spent; which model spent it is a fact
  // about the session, held in the Backend panel, and appending it made the one line that is
  // meant to be skimmed into the longest line in the footing.
  return parts.join("  ·  ");
}

// What a turn cost, as one quiet line. It is short enough to read in full, so there is nothing
// to fold away.
function usageLineNode(usage) {
  const line = document.createElement("span");
  line.className = "msg-usage";
  line.textContent = usageLine(usage);
  return line;
}

// The words a turn is made of, as one string. Only the prose blocks are here: a price, a tool
// call and a thought are drawn around the sentence rather than being part of it, so a copy
// that carried them would paste as a transcript of the machinery instead of what was said.
function messageText(blocks) {
  const parts = [];
  for (const block of blocks || []) {
    if (block.type !== BLOCK_TYPES.TEXT) continue;
    const text = String(block.content || "").trim();
    if (text) parts.push(text);
  }
  return parts.join("\n\n");
}

// Copy is a reading action, and the right button is where a reader already reaches for it. The
// old control turned into the word "Copied" in place; the menu has no room for that, so the
// confirmation is a line of system text the page already knows how to draw, and the menu closes
// on the press as it does for any other row.
function copyMessage(text) {
  if (!text) return true;
  // A page served over plain http has no clipboard API, so the older command is the fallback
  // rather than a failure the reader cannot do anything about.
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).catch(() => copyFallback(text));
    return true;
  }
  if (!copyFallback(text)) {
    createSystemMessage("Could not copy: the clipboard is not reachable from this page.");
    return false;
  }
  return true;
}

// A page served from an origin the clipboard API does not trust has no other way in: a field
// off-screen, selected and asked for the selection, is the command the browser still honours.
function copyFallback(text) {
  const field = document.createElement("textarea");
  field.value = text;
  field.setAttribute("readonly", "");
  field.style.position = "fixed";
  field.style.opacity = "0";
  document.body.appendChild(field);
  field.select();
  let ok = false;
  try {
    ok = document.execCommand("copy");
  } catch (e) {
    ok = false;
  }
  field.remove();
  return ok;
}

function createMessage(role, blocks, turn, text, usage) {
  const el = document.createElement("div");
  el.className = "msg " + role;
  // The words as the store has them, not as the page drew them: the editor offers a copy from
  // here, and a copy taken from the rendered DOM would flatten the reply's markdown back to
  // plain text and paste as a different sentence from the one that was written.
  el._puBlocks = blocks || [];

  // The bubble is what is drawn as a reply: the frame, the tint, and the room around the words.
  // It is a child of the message rather than the message itself so that the line under it can
  // sit outside it, on the page rather than on the paper -- see the footing below.
  const bubble = document.createElement("div");
  bubble.className = "msg-bubble";
  el.appendChild(bubble);

  const container = document.createElement("div");
  container.className = "blocks-container";
  bubble.appendChild(container);

  if (blocks) renderBlocks(blocks, container);

  // The footing closes the message: what it cost, on one line under the words. It is outside the
  // bubble, so the marked-up text and the note about the turn are two surfaces rather than one:
  // everything inside the frame is what was said, and everything under it is a fact about the
  // turn rather than something to do with it.
  // Who spoke is not written here. The two backgrounds already tell the two sides apart, and a
  // name saying what a colour has said four times down the page is a caption on a caption.
  // A message the reader cannot act on still gets no footing at all: a system remark is the page
  // talking, and an error is already something to be read.
  // What a reader *may* do with the message is not here: copying and editing are offered when
  // the bubble is clicked, in the same bar that carries Save and Cancel, so a reply at rest is
  // its words and nothing else.
  if (role === "assistant" && usage) {
    const foot = document.createElement("div");
    foot.className = "msg-foot";
    foot.appendChild(usageLineNode(usage));
    el.appendChild(foot);
  }

  // A turn of yours carries the turn number and the words it was drawn from, because what may be
  // done with it is asked for later: the right button reads these off the element, and `beginEdit`
  // finds the same two on the way in.
  if (role === "user" && typeof turn === "number" && turn > 0) {
    el.dataset.turn = String(turn);
    el.dataset.text = text || "";
  }

  // Read the position before the append, because the append itself moves the content
  // the reader is measured against: appending grows the scroll height without firing a
  // scroll event, so a view that only reacted to scrolling would never notice the row
  // it is now one bubble away from.
  const wasAtBottom = isAtBottom();
  PU.composer.messagesEl.appendChild(el);
  if (wasAtBottom) scrollToBottom();
  return el;
}

// The block list a message was drawn from, kept on the element so the editor can offer a copy of
// the words it is about to replace. It is read from the store the page already holds rather than
// from the rendered DOM: a copy of a reply with its markdown flattened back to text would paste
// as a different sentence from the one the model wrote.
function blocksOf(el) {
  const stored = el._puBlocks;
  if (Array.isArray(stored)) return stored;
  const text = el.dataset.text || "";
  return text ? [{ type: BLOCK_TYPES.TEXT, content: text }] : [];
}


