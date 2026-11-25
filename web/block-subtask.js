// SPDX-License-Identifier: GPL-3.0-only
// A task the agent handed to a sub-agent: which agent or directory was asked, what it was put
// to, and what came back. It folds like a tool call because that is what it is from the
// outside -- one step of a turn -- but what it holds is a whole conversation elsewhere, so it
// is read as its own kind rather than as an argument set.

"use strict";

// The `ask_dir` arguments, named the way the tool names them. One field names who answers —
// an agent in this table, or a directory that is a world of its own — and `prompt` is the
// whole question, so the question is never the thing that is missing. A record written before
// the two were one carries `agent` and `dir` instead, and is drawn the way it was meant.
function subtaskFields(args) {
  // A call whose arguments could not be parsed arrives as its raw string, which is the only
  // thing there is to show.
  if (typeof args === "string") return { who: "", agent: "", dir: "", prompt: args };
  if (!args || typeof args !== "object") {
    return { who: "", agent: "", dir: "", prompt: "" };
  }
  const agent = typeof args.agent === "string" ? args.agent : "";
  const dir = typeof args.dir === "string" ? args.dir : "";
  const ask = typeof args.ask === "string" ? args.ask : "";
  let prompt = typeof args.prompt === "string" ? args.prompt : "";
  if (!prompt) {
    // Stored before `prompt` had that name: the longest string is the question, not a path.
    const strings = Object.values(args).filter((v) => typeof v === "string" && v);
    prompt = strings.sort((a, b) => b.length - a.length)[0] || "";
  }
  return { who: ask, agent, dir, prompt };
}

class SubtaskBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-subtask";

    const header = document.createElement("div");

    this.labelEl = document.createElement("span");
    this.labelEl.className = "subtask-label";
    this.labelEl.textContent = "Subtask";
    this.dirEl = document.createElement("span");
    this.dirEl.className = "subtask-dir";
    this.summaryEl = document.createElement("span");
    this.summaryEl.className = "subtask-summary";
    this.statusEl = document.createElement("span");
    this.statusEl.className = "subtask-status";

    header.append(this.labelEl, this.dirEl, this.summaryEl, this.statusEl);
    buildCollapsible(this, header);
    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    this.dataset.subtaskId = block.id || "";
    const { who, agent, dir, prompt } = subtaskFields(block.args);
    // What the call was aimed at is the one fact that says which sub-agent ran, so it stays
    // in the header; the prompt is cut to a gist and read whole once the block is open.
    this.dirEl.textContent = who || dir || agent;
    this.dirEl.hidden = !who && !dir && !agent;
    const summary = oneLine(prompt);
    this.summaryEl.textContent = summary;
    this.summaryEl.hidden = !summary;
    this.statusEl.textContent = blockStatusText(block);
    syncCollapsible(this, block);
    this.#fillBody(block);
  }

  toggle() {
    toggleSelf(this);
  }

  #fillBody(block) {
    this.body.replaceChildren();
    const { who, agent, dir, prompt } = subtaskFields(block.args);
    if (who || agent || dir || prompt) {
      const asked = document.createElement("div");
      asked.className = "subtask-asked";
      if (who) {
        // One field named who answers, so it is shown once rather than split into an agent
        // line and a directory line that could disagree.
        asked.appendChild(labelledPre("subtask-dir-row", "Asked of", who, ""));
      } else {
        // A record written before the two were one: it named agent and directory separately,
        // and each of its fields is still what it said.
        if (agent) asked.appendChild(labelledPre("subtask-agent-row", "Agent", agent, ""));
        if (dir) asked.appendChild(labelledPre("subtask-dir-row", "Directory", dir, ""));
      }
      if (prompt) asked.appendChild(labelledPre("subtask-prompt-row", "Asked", prompt, ""));
      this.body.appendChild(asked);
    }
    if (block.status === "running" || block.status === "pending") return;
    // The reply came from a run of its own, so it is set as text: its markup is data.
    this.body.appendChild(labelledPre(
        "subtask-answer",
        block.error ? "Error" : "Answer",
        block.error || block.output || "(no answer)",
        block.error ? "subtask-error" : ""));
  }
}

customElements.define("pu-subtask", SubtaskBlock);
