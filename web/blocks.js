// SPDX-License-Identifier: GPL-3.0-only
// The kinds a transcript block comes in. They are data that more than one module reads —
// the fold that replays a stored turn, the stream that writes a live one, and the element each
// becomes — so they live in their own file rather than in the module that happens to be loaded
// first. A module that only folds history can then load this and nothing else.

"use strict";

const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  SUBTASK: "subtask",
  TEXT: "text",
};

// `ask_dir` is a tool call in the protocol and a sub-agent in what it holds: it
// runs a whole conversation in another directory and keeps only the answer. It is
// drawn as its own kind so the directory and the question are readable without
// opening the block, and so what came back is not mistaken for this turn's work.
const SUBTASK_TOOLS = new Set(["ask_dir"]);

function blockKindForTool(name) {
  return SUBTASK_TOOLS.has(name) ? BLOCK_TYPES.SUBTASK : BLOCK_TYPES.TOOL_CALL;
}
