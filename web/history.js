// SPDX-License-Identifier: GPL-3.0-only
// Folds a stored turn back into the bubbles a reader saw, so a replayed transcript is
// not a different shape from a live one.

"use strict";

// A string that is not JSON is kept as it came, so one unreadable call hides no other.
function parseToolArguments(raw) {
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw);
    } catch (_) {
      return raw;
    }
  }
  return raw === undefined ? null : raw;
}

// "running" is the stream's word and is never stored; the store holds answered, not
// answered yet, or interrupted before it could run.
function callStatus(statuses, index) {
  const stored = statuses[index];
  if (stored === "pending" || stored === "interrupted") return stored;
  return "done";
}

function thinkingText(msg) {
  if (!Array.isArray(msg.reasoning) || msg.reasoning.length === 0) return null;
  return msg.reasoning.map((block) => block.text || "").join("");
}

// The stored order is the order it happened in, so a block joins the one it follows only
// when that block is the same kind. A replay is then the same shape as the live stream.
function appendAtEnd(blocks, type, make) {
  const last = blocks[blocks.length - 1];
  if (last && last.type === type) return last;
  const block = make();
  blocks.push(block);
  return block;
}

function appendThinking(blocks, text) {
  const thinking = appendAtEnd(
      blocks,
      BLOCK_TYPES.THINKING,
      () => ({ type: BLOCK_TYPES.THINKING, content: "", collapsed: true }));
  thinking.content += text;
}

function toolCallBlock(call, status) {
  const fn = call.function || {};
  return {
    type: BLOCK_TYPES.TOOL_CALL,
    id: call.id || "unknown",
    name: fn.name || "unknown",
    args: parseToolArguments(fn.arguments),
    output: "",
    error: "",
    status,
    collapsed: true,
  };
}

// Parsed by the running turn; the raw content is what is left when it could not be.
function applyToolResult(blocks, msg) {
  const block = blocks.find((b) => b.type === BLOCK_TYPES.TOOL_CALL && b.id === msg.tool_call_id);
  if (!block) return;
  block.output = typeof msg.output === "string" ? msg.output : (msg.content || "");
  block.error = msg.error || "";
  block.status = "done";
}

function groupHistory(history) {
  const turns = [];
  let reply = null;

  const closeReply = () => {
    if (reply) turns.push(reply);
    reply = null;
  };
  const currentReply = () => {
    if (!reply) reply = { role: "assistant", blocks: [] };
    return reply;
  };

  for (const msg of history) {
    const content = msg.content || "";

    if (msg.role === "system") {
      closeReply();
      turns.push({ role: "system", text: content });
    } else if (msg.role === "user") {
      closeReply();
      turns.push({ role: "user", text: content, turn: msg.id, fromPeer: msg.from_peer });
    } else if (msg.role === "tool") {
      applyToolResult(currentReply().blocks, msg);
    } else if (msg.role === "assistant") {
      const bubble = currentReply();
      const text = thinkingText(msg);
      if (text) appendThinking(bubble.blocks, text);

      const calls = Array.isArray(msg.tool_calls) ? msg.tool_calls : [];
      const statuses = Array.isArray(msg.tool_call_status) ? msg.tool_call_status : [];
      calls.forEach((call, index) => {
        bubble.blocks.push(toolCallBlock(call, callStatus(statuses, index)));
      });

      if (content) {
        appendAtEnd(bubble.blocks, BLOCK_TYPES.TEXT, () => ({ type: BLOCK_TYPES.TEXT, content: "" }))
            .content += content;
      }

      // What this turn cost, beside it rather than inside it. The last assistant message of a
      // turn carries the turn's figure, so a later one replaces an earlier one rather than
      // adding to it.
      if (msg.usage) bubble.usage = msg.usage;
    }
  }

  closeReply();
  return turns;
}
