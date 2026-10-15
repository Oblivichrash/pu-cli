// SPDX-License-Identifier: GPL-3.0-only
// The live half of the transcript: the assistant message being written, the blocks it is
// made of, and the frame-coalesced re-render that keeps a token stream from re-parsing
// text and code already on screen.

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

function lastBlockOfType(type) {
  return PU.currentAssistantBlocks.findLast((b) => b.type === type) || null;
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
  if (!block.node) block.node = renderBlock(block);
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
  let block = lastBlockOfType(BLOCK_TYPES.TEXT);
  if (!block) {
    block = pushBlock({ type: BLOCK_TYPES.TEXT, content: "" });
  }
  block.content += payload.text || "";
  updateCurrentAssistantBlocks();
}

// Reasoning reads above the answer, in the order the history renderer puts it in.
function handleThinking(payload) {
  const text = payload.text || "";
  if (!text) return;
  let block = lastBlockOfType(BLOCK_TYPES.THINKING);
  if (!block) {
    block = { type: BLOCK_TYPES.THINKING, content: "", collapsed: false };
    PU.currentAssistantBlocks.unshift(block);
  }
  block.content += text;
  updateCurrentAssistantBlocks(false);
}
