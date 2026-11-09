// SPDX-License-Identifier: GPL-3.0-only
// A task the agent handed to a sub-agent: which agent or directory was asked, what it was put
// to, and what came back. It folds like a tool call because that is what it is from the
// outside -- one step of a turn -- but what it holds is a whole conversation elsewhere, so it
// is read as its own kind rather than as an argument set.

"use strict";

const SUBTASK_PROMPT_MAX = 60;

function subtaskOneLine(text) {
  const flat = String(text ?? "").replace(/\s+/g, " ").trim();
  return flat.length > SUBTASK_PROMPT_MAX ? flat.slice(0, SUBTASK_PROMPT_MAX) + "…" : flat;
}

// The `ask_dir` arguments, named the way the tool names them. The tool is aimed one of two
// ways — a name picks an agent already in this table, a directory picks a world of its own —
// and `prompt` is required under both, so the question is never the thing that is missing.
function subtaskFields(args) {
  // A call whose arguments could not be parsed arrives as its raw string, which is the only
  // thing there is to show.
  if (typeof args === "string") return { agent: "", dir: "", prompt: args };
  if (!args || typeof args !== "object") return { agent: "", dir: "", prompt: "" };
  const agent = typeof args.agent === "string" ? args.agent : "";
  const dir = typeof args.dir === "string" ? args.dir : "";
  let prompt = typeof args.prompt === "string" ? args.prompt : "";
  if (!prompt) {
    // Stored before `prompt` had that name: the longest string is the question, not a path.
    const strings = Object.values(args).filter((v) => typeof v === "string" && v);
    prompt = strings.sort((a, b) => b.length - a.length)[0] || "";
  }
  return { agent, dir, prompt };
}

function labelledBlock(wrapperClass, label, text, preClass) {
  const wrapper = document.createElement("div");
  wrapper.className = wrapperClass;
  const strong = document.createElement("strong");
  strong.textContent = label;
  const pre = document.createElement("pre");
  if (preClass) pre.className = preClass;
  pre.textContent = text;
  wrapper.append(strong, pre);
  return wrapper;
}

class SubtaskBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-subtask";

    const header = document.createElement("div");

    this.labelEl = document.createElement("span");
    this.labelEl.className = "subtask-label";
    this.labelEl.textContent = "子任务";
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
    const { agent, dir, prompt } = subtaskFields(block.args);
    // What the call was aimed at is the one fact that says which sub-agent ran, so it stays
    // in the header; the prompt is cut to a gist and read whole once the block is open.
    this.dirEl.textContent = dir || agent;
    this.dirEl.hidden = !dir && !agent;
    const summary = subtaskOneLine(prompt);
    this.summaryEl.textContent = summary;
    this.summaryEl.hidden = !summary;
    this.statusEl.textContent = this.#statusText(block);
    syncCollapsible(this, block);
    this.#fillBody(block);
  }

  toggle() {
    toggleSelf(this);
  }

  #statusText(block) {
    if (block.status === "running") return "running…";
    if (block.status === "pending") return "no result";
    if (block.status === "interrupted") return "interrupted";
    return block.error ? "failed" : "";
  }

  #fillBody(block) {
    this.body.replaceChildren();
    const { agent, dir, prompt } = subtaskFields(block.args);
    if (agent || dir || prompt) {
      const asked = document.createElement("div");
      asked.className = "subtask-asked";
      if (agent) asked.appendChild(labelledBlock("subtask-agent-row", "Agent", agent, ""));
      if (dir) asked.appendChild(labelledBlock("subtask-dir-row", "Directory", dir, ""));
      if (prompt) asked.appendChild(labelledBlock("subtask-prompt-row", "Asked", prompt, ""));
      this.body.appendChild(asked);
    }
    if (block.status === "running" || block.status === "pending") return;
    // The reply came from a run of its own, so it is set as text: its markup is data.
    this.body.appendChild(labelledBlock(
        "subtask-answer",
        block.error ? "Error" : "Answer",
        block.error || block.output || "(no answer)",
        block.error ? "subtask-error" : ""));
  }
}

customElements.define("pu-subtask", SubtaskBlock);
