// SPDX-License-Identifier: GPL-3.0-only
// Which element a block becomes, kept apart from the elements themselves so the
// transcript can grow a block kind without any renderer learning about the others.

"use strict";

const BLOCK_NODES = {
  [BLOCK_TYPES.THINKING]: ThinkingBlock,
  [BLOCK_TYPES.TOOL_CALL]: ToolBlock,
  [BLOCK_TYPES.SUBTASK]: SubtaskBlock,
  [BLOCK_TYPES.TEXT]: TextBlock,
};

// The element builds its DOM when it is connected, so the block is handed over here and
// read back once the node is in the tree; nothing is written to a detached node.
function createBlockNode(block) {
  const Node = BLOCK_NODES[block.type];
  if (!Node) return null;
  const node = new Node();
  node._block = block;
  return node;
}

// A block that folds marks itself, so this names no kind and a new one folds by saying so.
function toggleBlock(headerEl) {
  const wrapper = headerEl.parentElement;
  if (wrapper && wrapper.collapsible) wrapper.toggle();
}

// A folding block is a header, a body and the same two halves in every element, so the
// parts and the folded state are built and re-read here rather than in each one.
function buildCollapsible(wrapper, header) {
  header.className = "block-header";
  header.onclick = () => toggleBlock(header);

  const icon = document.createElement("span");
  icon.className = "toggle-icon";
  header.appendChild(icon);

  const body = document.createElement("div");
  body.className = "block-body";

  wrapper.append(header, body);
  wrapper.body = body;
  wrapper.iconEl = icon;
  wrapper.collapsible = true;
}

function syncCollapsible(wrapper, block) {
  wrapper.body.classList.toggle("collapsed", !!block.collapsed);
  wrapper.iconEl.textContent = block.collapsed ? "▶" : "▼";
}

function toggleSelf(wrapper) {
  wrapper._block.collapsed = !wrapper._block.collapsed;
  syncCollapsible(wrapper, wrapper._block);
}

// What a folded card says in its header about the step's state. A finished call that worked says
// so by being finished, so only the states a reader cannot infer are named: still running, not
// answered, and stopped before it ran.
function blockStatusText(block) {
  if (block.status === "running") return "running…";
  if (block.status === "pending") return "no result";
  if (block.status === "interrupted") return "interrupted";
  return block.error ? "failed" : "";
}

// A line of free text cut to what a header has room for. The two cards that quote an argument
// this way want the same length, so the limit lives here once.
const ONE_LINE_MAX = 60;

function oneLine(text, max = ONE_LINE_MAX) {
  const flat = String(text ?? "").replace(/\s+/g, " ").trim();
  return flat.length > max ? flat.slice(0, max) + "…" : flat;
}

// A labelled block of preformatted text: a name, then the text under it. Both cards that fold
// a step hold two or three of these, and the shape is the same one.
function labelledPre(wrapperClass, label, text, preClass) {
  const wrapper = document.createElement("div");
  wrapper.className = wrapperClass;
  const strong = document.createElement("strong");
  strong.textContent = label;
  const pre = document.createElement("pre");
  if (preClass) pre.className = preClass;
  pre.textContent = text;
  wrapper.append(strong, pre);
  return wrapper;
}

// A block list is a timeline, so an increment joins the block it follows only when that block
// is the same kind. Anything else starts a new block at the end: reasoning that arrives after a
// tool call belongs after it, not folded into the reasoning above. The live stream and a replay
// of the store both build their lists this way, which is what keeps a replayed transcript the
// same shape as one that arrived.
function blockAtEnd(blocks, type, make) {
  const last = blocks[blocks.length - 1];
  if (last && last.type === type) return last;
  const block = make();
  blocks.push(block);
  return block;
}
