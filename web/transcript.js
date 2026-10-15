// SPDX-License-Identifier: GPL-3.0-only
// The live half of the transcript: the assistant message being written and the blocks it
// is made of.

"use strict";

let assistantRenderQueued = false;
let pendingScrollFollow = false;

function startAssistantMessage() {
  PU.currentAssistantBlocks = [];
  PU.currentAssistantEl = createMessage("assistant", PU.currentAssistantBlocks);
  PU.currentAssistantEl.classList.add("typing");
}

function finishAssistantMessage() {
  if (!PU.currentAssistantEl) return;
  PU.currentAssistantEl.classList.remove("typing");
  PU.currentAssistantEl = null;
  PU.currentAssistantBlocks = [];
}

function removeCurrentAssistantMessage() {
  if (!PU.currentAssistantEl) return;
  PU.currentAssistantEl.remove();
  PU.currentAssistantEl = null;
  PU.currentAssistantBlocks = [];
}

// A block list is a timeline, so an increment joins the block it follows only when that
// block is the same kind. Anything else starts a new block at the end: reasoning that
// arrives after a tool call belongs after it, not folded into the reasoning above.
function blockAtEnd(type, make) {
  const blocks = PU.currentAssistantBlocks;
  const last = blocks[blocks.length - 1];
  if (last && last.type === type) return last;
  return pushBlock(make());
}

function pushBlock(block) {
  PU.currentAssistantBlocks.push(block);
  updateCurrentAssistantBlocks();
  return block;
}

function updateCurrentAssistantBlocks(stickToBottom) {
  // A following update must not be lost to one already queued that would not follow.
  if (stickToBottom !== false) pendingScrollFollow = true;
  if (assistantRenderQueued) return;
  assistantRenderQueued = true;
  requestAnimationFrame(() => {
    assistantRenderQueued = false;
    const follow = pendingScrollFollow;
    pendingScrollFollow = false;
    renderAssistantBlocks(follow);
  });
}

function syncBlockNode(block, container, index) {
  // A node already in the container is patched; a new one is inserted, and its own
  // connectedCallback draws it from the block it was handed.
  if (!block.node) block.node = createBlockNode(block);
  if (container.childNodes[index] !== block.node) {
    container.insertBefore(block.node, container.childNodes[index] || null);
  } else {
    block.node.sync(block);
  }
}

function renderAssistantBlocks(stickToBottom) {
  if (!PU.currentAssistantEl) return;
  const container = PU.currentAssistantEl.querySelector(".blocks-container");
  if (!container) return;

  PU.currentAssistantBlocks.forEach((block, i) => syncBlockNode(block, container, i));

  while (container.childNodes.length > PU.currentAssistantBlocks.length) {
    container.removeChild(container.lastChild);
  }
  // A reasoning chunk is not the answer arriving, so it does not pull the view down.
  if (stickToBottom !== false && PU.isAtBottom) {
    PU.composer.messagesEl.scrollTop = PU.composer.messagesEl.scrollHeight;
  }
}

function handleToolStart(payload) {
  pushBlock({
    type: BLOCK_TYPES.TOOL_CALL,
    id: payload.id,
    name: payload.name,
    args: payload.args,
    output: "",
    error: "",
    status: "running",
    collapsed: true,
  });
}

function handleToolEnd(payload) {
  const block = PU.currentAssistantBlocks.find(
      (b) => b.type === BLOCK_TYPES.TOOL_CALL && b.id === payload.id);
  if (!block) return;
  block.output = payload.output || "";
  block.error = payload.error || "";
  block.status = "done";
  updateCurrentAssistantBlocks();
}

function handleChunk(payload) {
  const block = blockAtEnd(BLOCK_TYPES.TEXT, () => ({ type: BLOCK_TYPES.TEXT, content: "" }));
  block.content += payload.text || "";
  updateCurrentAssistantBlocks();
}

function handleThinking(payload) {
  const text = payload.text || "";
  if (!text) return;
  const block = blockAtEnd(
      BLOCK_TYPES.THINKING,
      () => ({ type: BLOCK_TYPES.THINKING, content: "", collapsed: true }));
  block.content += text;
  updateCurrentAssistantBlocks();
}
