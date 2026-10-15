// SPDX-License-Identifier: GPL-3.0-only
// Which element a block becomes, kept apart from the elements themselves so the
// transcript can grow a block kind without any renderer learning about the others.

"use strict";

const BLOCK_NODES = {
  [BLOCK_TYPES.THINKING]: ThinkingBlock,
  [BLOCK_TYPES.TOOL_CALL]: ToolBlock,
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
