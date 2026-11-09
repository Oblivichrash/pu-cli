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
// back. They live where the editor does, under the field, because a message has one moment at
// which a reader acts on it and that is the moment they click it. Under the bubble they were
// always there: on a page of twenty turns, twenty rows of marks the eye had to step over to
// reach the next reply.
// `Save` and `Cancel` are built by `beginEdit` and appended after this row, so the bar reads
// copy, edit, then the two that end the edit.
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
    // Already open is not a second edit: the bar the reader is pressing in is the sign that it
    // happened, and reopening it would throw away what they had typed so far.
    edit.addEventListener("click", (event) => {
      event.stopPropagation();
      if (el.dataset.editing !== "1") beginEdit(el, event, true);
    });
    actions.appendChild(edit);
  }

  return actions;
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

  // A turn of yours is a thing you may have meant differently, and its text carries the same
  // affordance the editor offers: the bubble stays where it is and the words become editable in
  // place. The editor is the named way to do it; the text is the way a reader reaches for
  // without looking.
  if (role === "user" && typeof turn === "number" && turn > 0) {
    el.classList.add("editable");
    el.dataset.turn = String(turn);
    el.dataset.text = text || "";
    el.title = "Click to edit this message";
    el.addEventListener("click", (event) => beginEdit(el, event));
  }

  // A reply has nothing to change and no turn to take back, so clicking it opens the one bar of
  // controls it has rather than an editor: a reader who wants the reply somewhere else has to
  // ask for it, and asking for it is a click. Without this the reply would have no way to be
  // copied at all, since its footing now carries only what the turn cost.
  if (role === "assistant") {
    el.classList.add("copyable");
    el.addEventListener("click", (event) => toggleReplyControls(el, event));
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

// A reply's controls are opened by a click and closed by the next one, or by a click anywhere
// else on the page. It is one bar under a message that cannot be edited, so there is no field
// for it to share a row with and nothing to save: copy is the whole of what a reply offers.
// The bar goes inside the bubble rather than under it, in the space the reader's click opened.
function toggleReplyControls(el, event) {
  // A link inside the reply is the reader following the reply, not asking for its controls, and
  // a selection is a reader taking part of it by hand.
  if (event && event.target.closest("a, button")) return;
  if (event && String(window.getSelection() || "").length > 0) return;

  const open = el.querySelector(":scope > .msg-bubble > .msg-actions");
  if (open) {
    closeReplyControls(el);
    return;
  }
  closeAllReplyControls(el);

  const bar = messageActions(el, "assistant", undefined, blocksOf(el));
  bar.classList.add("reply-bar");
  (el.querySelector(".msg-bubble") || el).appendChild(bar);
  el.classList.add("showing-controls");
}

function closeReplyControls(el) {
  el.querySelectorAll(":scope > .msg-bubble > .msg-actions.reply-bar").forEach((bar) => bar.remove());
  el.classList.remove("showing-controls");
}

function closeAllReplyControls(except) {
  (PU.composer.messagesEl || document).querySelectorAll(".msg.assistant.showing-controls").forEach((other) => {
    if (other !== except) closeReplyControls(other);
  });
}

// One listener for the whole page rather than one per reply: a click that lands anywhere but on
// a reply's controls is a reader moving on, and a bar left open behind them would be a message
// still asking to be acted on after the attention went elsewhere. It runs in the capture phase
// so a handler inside the page cannot stop it from being heard.
document.addEventListener(
  "click",
  (event) => {
    if (event.target.closest && event.target.closest(".msg.assistant")) return;
    closeAllReplyControls(null);
  },
  true
);
