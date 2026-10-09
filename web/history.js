// SPDX-License-Identifier: GPL-3.0-only
//
// Rebuilding the transcript from the stored conversation: the store holds one node per
// provider message while the transcript shows one bubble per turn, so a reload groups
// them the way the streaming path builds them — a result folded back into the call it
// answers, everything between two user turns one reply. Pure, so the grouping can be
// exercised without a browser.

export const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  TEXT: "text",
};

// Provider arguments travel as an object or as an encoded string. A string that is
// not JSON is kept as it came: a call whose arguments cannot be read is still a
// call the reader has to see, and one unreadable call must not hide the rest of
// the turn, which is what a throw in the middle of the loop would do.
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

// What the store recorded: a result answered the call, or nothing has. "running"
// is the stream's word for a call in flight and is never stored.
function callStatus(statuses, index) {
  return statuses[index] === "pending" ? "pending" : "done";
}

export function groupHistory(history) {
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
    const role = msg.role;
    const content = msg.content || "";

    if (role === "system") {
      closeReply();
      turns.push({ role: "system", text: content });
      continue;
    }

    if (role === "user") {
      closeReply();
      turns.push({ role: "user", text: content, turn: msg.id });
      continue;
    }

    if (role === "tool") {
      const bubble = currentReply();
      const block = bubble.blocks.find(
        (b) => b.type === BLOCK_TYPES.TOOL_CALL && b.id === msg.tool_call_id
      );
      if (!block) continue;
      // `output` and `error` are the result as the running turn parsed it; the raw
      // content is what is left when the server could not parse it either.
      block.output = typeof msg.output === "string" ? msg.output : content;
      block.error = msg.error || "";
      block.status = "done";
      continue;
    }

    if (role !== "assistant") continue;

    const bubble = currentReply();

    if (msg.reasoning_content) {
      // One thinking block per reply, above the rest: that is where the streaming
      // path puts it, and where it appends the reasoning of every iteration a
      // tool-using turn went through.
      let thinking = bubble.blocks.find((b) => b.type === BLOCK_TYPES.THINKING);
      if (!thinking) {
        thinking = { type: BLOCK_TYPES.THINKING, content: "", collapsed: true };
        bubble.blocks.unshift(thinking);
      }
      thinking.content += msg.reasoning_content;
    }

    const calls = Array.isArray(msg.tool_calls) ? msg.tool_calls : [];
    const statuses = Array.isArray(msg.tool_call_status) ? msg.tool_call_status : [];
    calls.forEach((call, index) => {
      const fn = call.function || {};
      bubble.blocks.push({
        type: BLOCK_TYPES.TOOL_CALL,
        id: call.id || "unknown",
        name: fn.name || "unknown",
        args: parseToolArguments(fn.arguments),
        output: "",
        error: "",
        status: callStatus(statuses, index),
        collapsed: true,
      });
    });

    if (content) bubble.blocks.push({ type: BLOCK_TYPES.TEXT, content });
  }

  closeReply();
  return turns;
}
