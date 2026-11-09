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

// The mark a control wears. The page draws its other controls as bare glyphs in the same family
// -- a caret for the servers, a block for the panel, a cross to dismiss -- so a message's two
// controls are drawn rather than spelled, and the word they stand for stays as their accessible
// name. The word a copy turns into, "Copied", is the one place a word is still shown, because a
// copy that says nothing looks exactly like a copy that failed.
//
// These are drawn as paths rather than set as characters, because a character's shape is whatever
// the resolved font decides it is. The body stack on this page is a CJK serif, and dingbats are
// among the codepoints where the CJK and the Latin designs disagree: U+270E in the CJK face leans
// with its tip one way and in a Latin face leans the other, so the same markup rendered as a
// different mark depending on which fallback happened to answer. A path cannot drift that way.
// It also settles the size: a dingbat carries the font's own side bearings and em placement, so
// two of them set at 16px sit at slightly different heights in the same row.
//
// Both are a single weight of stroke with no interior detail, which is what survives being drawn
// at 16px on a tinted background. The copy mark used to be U+2398, a dotted frame with an arrow
// through it: four ideas in a box 16px wide is three too many, and at that size it read as a
// smudge rather than as a sheet going somewhere. Two sheets offset from each other say "copy"
// with nothing to spare.
//
// The pencil points down-left, the angle it is held at and the angle the word means. The glyph it
// replaced, U+270F, is drawn lying on its side and reads as a crayon at rest rather than as a tool
// in use.
const CONTROL_ICONS = {
  // A sheet laid over another: the one behind is open on its two hidden edges, so the pair reads
  // as two sheets rather than as one frame with a line in it.
  copy: "M5.5 2.5h8v8M10.5 13.5h-8v-8h8Z",
  // A pencil, tip down-left: the body runs in from the upper right and closes to a point, with a
  // cut across the shaft marking where the wood ends and the graphite begins.
  edit: "M12.9 1.9a1.5 1.5 0 0 1 2.1 2.1L6.2 12.8 1.8 14.2 3.2 9.8ZM2.9 13.1 5.1 10.9M10.9 3.9 13.1 6.1",
};

// The two shapes are drawn as one <svg> each so the button keeps a single child to swap in and
// out, and so the stroke width and cap style are set in one place for both.
function controlIcon(name) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("width", "16");
  svg.setAttribute("height", "16");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.75");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("focusable", "false");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", CONTROL_ICONS[name]);
  svg.appendChild(path);
  return svg;
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
      button.textContent = "";
      button.appendChild(controlIcon("copy"));
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
  copy.appendChild(controlIcon("copy"));
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
    edit.appendChild(controlIcon("edit"));
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
