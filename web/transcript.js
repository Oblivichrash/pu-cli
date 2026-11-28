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
    type: blockKindForTool(payload.name),
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
      (b) => (b.type === BLOCK_TYPES.TOOL_CALL || b.type === BLOCK_TYPES.SUBTASK) &&
          b.id === payload.id);
  if (!block) {
    // A snapshot carries a tool's end, so a turn that arrives whole never sees its start: the
    // result carries the call's own name and arguments for that reason, and the block is drawn
    // from the end rather than lost.
    pushBlock({
      type: blockKindForTool(payload.name),
      id: payload.id,
      name: payload.name || "",
      args: payload.args ?? null,
      output: payload.output || "",
      error: payload.error || "",
      page: payload.page ?? null,
      status: "done",
      collapsed: true,
    });
    return;
  }
  block.output = payload.output || "";
  block.error = payload.error || "";
  block.page = payload.page ?? null;
  block.status = "done";
  updateCurrentAssistantBlocks();
}

function handleChunk(payload) {
  const block = blockAtEnd(
      PU.currentAssistantBlocks, BLOCK_TYPES.TEXT, () => ({ type: BLOCK_TYPES.TEXT, content: "" }));
  block.content += payload.text || "";
  updateCurrentAssistantBlocks();
}

function handleThinking(payload) {
  const text = payload.text || "";
  if (!text) return;
  const block = blockAtEnd(
      PU.currentAssistantBlocks,
      BLOCK_TYPES.THINKING,
      () => ({ type: BLOCK_TYPES.THINKING, content: "", collapsed: true }));
  block.content += text;
  updateCurrentAssistantBlocks();
}

// A turn handed over whole: the server sends the state of the turn in flight and of the last
// finished one when a page attaches, not the frames that built them. A chunk appends to a turn
// and a snapshot replaces it, so a turn drawn once on connect and handed over again on the next
// reconnect is still one reply on the page -- which is why nothing has to be held back while the
// page catches up, and why a snapshot of a finished turn can be ignored outright.
function handleTurnSnapshot(payload) {
  if (!payload || payload.finished) return;
  const blocks = blocksFromSnapshot(payload);
  if (!PU.currentAssistantEl) {
    if (blocks.length === 0) return;
    PU.currentAssistantBlocks = [];
    PU.currentAssistantEl = createMessage("assistant", PU.currentAssistantBlocks);
    PU.currentAssistantEl.classList.add("typing");
  }
  PU.currentAssistantBlocks.length = 0;
  for (const block of blocks) PU.currentAssistantBlocks.push(block);
  updateCurrentAssistantBlocks();
}

// The snapshot's fields in the order a live turn draws them. A tool is one block whether it is
// running or done, so the two lists are walked as one.
function blocksFromSnapshot(snapshot) {
  const blocks = [];
  const thinking = snapshot.thinking || "";
  if (thinking) {
    blocks.push({ type: BLOCK_TYPES.THINKING, content: thinking, collapsed: true });
  }
  for (const tool of snapshot.tools || []) {
    const done = tool.state === "done";
    blocks.push({
      type: blockKindForTool(tool.name),
      id: tool.id,
      name: tool.name || "unknown",
      args: tool.args ?? null,
      output: done ? (tool.output || "") : "",
      error: done ? (tool.error || "") : "",
      page: done ? (tool.page ?? null) : null,
      status: done ? "done" : "running",
      collapsed: true,
    });
  }
  const text = snapshot.text || "";
  if (text) blocks.push({ type: BLOCK_TYPES.TEXT, content: text });
  return blocks;
}
