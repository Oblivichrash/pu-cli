// SPDX-License-Identifier: GPL-3.0-only
// The three kinds a transcript block comes in. They are data that more than one module reads —
// the fold that replays a stored turn, the stream that writes a live one, and the element each
// becomes — so they live in their own file rather than in the module that happens to be loaded
// first. A module that only folds history can then load this and nothing else.

"use strict";

const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  TEXT: "text",
};
