// SPDX-License-Identifier: GPL-3.0-only
// The live half of the transcript: the assistant message being written and the blocks it
// is made of.

"use strict";

// The frame a redraw is waiting on, or null when none is queued. It is an id rather than a flag
// because a redraw that is no longer wanted has to be cancelled: a callback left in the queue
// runs against a bubble that has since been finished or removed.
let assistantRenderFrame = null;
let pendingScrollFollow = false;

function startAssistantMessage() {
  PU.currentAssistantBlocks = [];
  PU.currentAssistantEl = createMessage("assistant", PU.currentAssistantBlocks);
  PU.currentAssistantEl.classList.add("typing");
}

// The content has to be on the page before the bubble stops being the live one. A chunk and the
// `done` that follows it arrive in the same tick, so the last chunk only ever sits in the block
// list while its redraw waits for the next frame — and `done` used to drop the element first,
// which left the answer on screen as a prefix of what the store holds.
function finishAssistantMessage() {
  flushAssistantRender();
  if (!PU.currentAssistantEl) return;
  PU.currentAssistantEl.classList.remove("typing");
  PU.currentAssistantEl = null;
  PU.currentAssistantBlocks = [];
}

// A bubble that is being discarded has nothing to draw, so its queued redraw is cancelled rather
// than run against an element that is gone.
function removeCurrentAssistantMessage() {
  discardAssistantRender();
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
  if (assistantRenderFrame !== null) return;
  assistantRenderFrame = requestAnimationFrame(() => {
    assistantRenderFrame = null;
    const follow = pendingScrollFollow;
    pendingScrollFollow = false;
    renderAssistantBlocks(follow);
  });
}

// Draws what is queued now, rather than at the next frame. The frame a tab is not painting never
// arrives, so a page in the background would otherwise come back to a bubble that stopped at the
// point it was hidden — and a redraw waiting then would still be waiting.
function flushAssistantRender() {
  if (assistantRenderFrame === null) return;
  cancelAnimationFrame(assistantRenderFrame);
  assistantRenderFrame = null;
  const follow = pendingScrollFollow;
  pendingScrollFollow = false;
  renderAssistantBlocks(follow);
}

function discardAssistantRender() {
  if (assistantRenderFrame === null) return;
  cancelAnimationFrame(assistantRenderFrame);
  assistantRenderFrame = null;
  pendingScrollFollow = false;
}

// A hidden tab is not painted, so a redraw it holds waits for the reader to come back and the
// reply stops where they left it until then. Flushing on the way out puts the content in the DOM
// now rather than whenever the browser next paints, so coming back shows the whole answer and not
// the point it was interrupted at.
document.addEventListener("visibilitychange", () => {
  if (document.hidden) flushAssistantRender();
});

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

  // A reasoning chunk is not the answer arriving, so it does not pull the view down.
  const follow = stickToBottom !== false && isAtBottom();

  PU.currentAssistantBlocks.forEach((block, i) => syncBlockNode(block, container, i));

  while (container.childNodes.length > PU.currentAssistantBlocks.length) {
    container.removeChild(container.lastChild);
  }
  if (follow) scrollToBottom();
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
  if (!block) {
    // A page that reconnected mid-command is replayed the end without the start: the frames
    // before the reconnect were drawn and dropped. The result carries the call's own name and
    // arguments for that reason, so the block is drawn from the end rather than lost.
    pushBlock({
      type: BLOCK_TYPES.TOOL_CALL,
      id: payload.id,
      name: payload.name || "",
      args: payload.args ?? null,
      output: payload.output || "",
      error: payload.error || "",
      status: "done",
      collapsed: true,
    });
    return;
  }
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
