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
  if (usage.model) parts.push(usage.model);
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

// The mark a control wears. The page draws its other controls as bare glyphs in the same family
// -- a caret for the servers, a block for the panel, a cross to dismiss -- so a message's two
// controls are drawn rather than spelled, and the word they stand for stays as their accessible
// name. The letter changed to "Copied" is the one place a word is still shown, because a copy
// that says nothing looks exactly like a copy that failed.
const CONTROL_GLYPHS = {
  copy: "\u2398",      // ⎘ -- a sheet going to another sheet
  edit: "\u270E",      // ✎
};

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

// Copy is a reading action, so it is offered on both sides: a reply is the thing most often
// taken somewhere else, and a question is what it gets quoted back to. The label is the page's
// only confirmation, on the button that was pressed rather than in a strip elsewhere, because a
// notice about a copy is about a copy and nothing else on the page changed.
// Only the words are replaced, never the accessible name: the state is a remark about the press,
// and a control whose announcable name changed under the reader would be a different control.
function copyMessage(text, button) {
  if (!text) return;
  const settle = (ok) => {
    button.textContent = ok ? "Copied" : "Failed";
    button.classList.add("said");
    clearTimeout(button.copyTimer);
    button.copyTimer = setTimeout(() => {
      button.textContent = CONTROL_GLYPHS.copy;
      button.classList.remove("said");
    }, 1400);
  };
  // A page served over plain http has no clipboard API, so the older command is the fallback
  // rather than a failure the reader cannot do anything about.
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(() => settle(true), () => settle(copyFallback(text)));
    return;
  }
  settle(copyFallback(text));
}

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

// The controls a message carries: copy on both sides, and edit only where a turn can be taken
// back. They close the footing under the bubble, pushed to its far edge, so everything a reader
// may do with a message is in one place and the name and the cost are left to read as a remark.
function messageActions(el, role, turn, blocks) {
  const actions = document.createElement("span");
  actions.className = "msg-actions";

  const copy = document.createElement("button");
  copy.type = "button";
  copy.className = "msg-action msg-copy";
  copy.textContent = CONTROL_GLYPHS.copy;
  copy.title = "Copy this message";
  copy.setAttribute("aria-label", "Copy this message");
  copy.addEventListener("click", (event) => {
    event.stopPropagation();
    copyMessage(messageText(blocks), copy);
  });
  actions.appendChild(copy);

  if (role === "user" && typeof turn === "number" && turn > 0) {
    const edit = document.createElement("button");
    edit.type = "button";
    edit.className = "msg-action msg-edit";
    edit.textContent = CONTROL_GLYPHS.edit;
    edit.title = "Edit this message";
    edit.setAttribute("aria-label", "Edit this message");
    edit.addEventListener("click", (event) => {
      event.stopPropagation();
      beginEdit(el, event, true);
    });
    actions.appendChild(edit);
  }

  return actions;
}

function createMessage(role, blocks, turn, text, usage) {
  const el = document.createElement("div");
  el.className = "msg " + role;

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

  // The footing closes the message: what it cost, and what may be done with it, on one line
  // under the words. It is outside the bubble, so the marked-up text and the note about the
  // turn are two surfaces rather than one: everything inside the frame is what was said, and
  // everything under it is what the reader may do about it.
  // Who spoke is not written here. The two backgrounds already tell the two sides apart, and a
  // name saying what a colour has said four times down the page is a caption on a caption.
  // A message the reader cannot act on still gets no footing at all: a system remark is the page
  // talking, and an error is already something to be read.
  if (role === "user" || role === "assistant") {
    const foot = document.createElement("div");
    foot.className = "msg-foot";

    if (role === "assistant" && usage) foot.appendChild(usageLineNode(usage));

    // The controls go last and are pushed to the far edge, so the cost reads as one remark at
    // the left and the things that can be pressed sit where a hand already is.
    foot.appendChild(messageActions(el, role, turn, blocks));
    el.appendChild(foot);
  }

  // A turn of yours is a thing you may have meant differently, and its text carries the same
  // affordance the Edit button offers: the bubble stays where it is and the words become
  // editable in place. The row above is the named way to do it; the text is the way a reader
  // reaches for without looking.
  if (role === "user" && typeof turn === "number" && turn > 0) {
    el.classList.add("editable");
    el.dataset.turn = String(turn);
    el.dataset.text = text || "";
    el.title = "Click to edit this message";
    el.addEventListener("click", (event) => beginEdit(el, event));
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
