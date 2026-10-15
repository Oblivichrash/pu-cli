// SPDX-License-Identifier: GPL-3.0-only
// A tool call's card: what was asked, what came back, and the one action that belongs to
// a command still running. The status is itself the answer to "what happened here", so
// `sync` derives it rather than storing it twice.

"use strict";

function labelledPre(wrapperClass, label, text, preClass) {
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

class ToolBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-tool";

    const header = document.createElement("div");
    header.className = "block-header";
    header.onclick = () => toggleBlock(header);

    this.nameEl = document.createElement("span");
    this.statusEl = document.createElement("span");
    this.statusEl.className = "tool-status";
    this.stopBtn = document.createElement("button");
    this.stopBtn.type = "button";
    this.stopBtn.className = "stop-command";
    this.stopBtn.textContent = "■ Stop";
    this.stopBtn.title = "Stop this command";
    this.stopBtn.onclick = (e) => {
      e.stopPropagation();
      askStopNote(this, this._block.id);
    };

    this.iconEl = document.createElement("span");
    this.iconEl.className = "toggle-icon";
    header.append(this.nameEl, this.statusEl, this.stopBtn, this.iconEl);

    this.body = document.createElement("div");
    this.body.className = "block-body";

    this.append(header, this.body);
    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    this.dataset.toolId = block.id || "";
    this.nameEl.textContent = "🔧 " + (block.name || "unknown");
    this.statusEl.textContent = this.#statusText(block);
    // The stop sits on the block it would kill, so the thing stopped is the thing on screen.
    this.stopBtn.hidden = !(block.status === "running" && block.id);
    this.iconEl.textContent = block.collapsed ? "▶" : "▼";
    this.body.classList.toggle("collapsed", !!block.collapsed);
    this.#fillBody(block);
  }

  toggle() {
    this._block.collapsed = !this._block.collapsed;
    this.sync(this._block);
  }

  toggled() {
    return this._block.collapsed;
  }

  #statusText(block) {
    if (block.status === "running") return " ⏳ Running...";
    // Stored with no result: the turn ended before one was written, so what the
    // call did is unknown rather than successful.
    if (block.status === "pending") return " ⚠ No result";
    return block.error ? " ❌ Failed" : " ✅ Done";
  }

  #fillBody(block) {
    this.body.replaceChildren();
    // Tool output is a command's raw product, so it is set as text: its markup is data.
    this.body.appendChild(
        labelledPre("tool-args", "Arguments", JSON.stringify(block.args ?? null, null, 2), ""));
    if (block.status !== "done") return;
    this.body.appendChild(labelledPre(
        "tool-result",
        block.error ? "Error" : "Output",
        block.error || block.output || "(no output)",
        block.error ? "tool-error" : ""));
  }
}

// The note is written against the command it explains, not in a modal. Enter stops, Esc does not.
function askStopNote(wrapper, toolId) {
  if (wrapper.querySelector(".stop-note")) return;
  const body = wrapper.querySelector(".block-body");
  if (!body) return;
  body.classList.remove("collapsed");
  const row = document.createElement("div");
  row.className = "stop-note";
  const input = document.createElement("input");
  input.type = "text";
  input.placeholder = "Why stop it? (Enter to stop, Esc to keep running)";
  const confirm = document.createElement("button");
  confirm.type = "button";
  confirm.textContent = "Stop";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.className = "stop-note-cancel";
  cancel.textContent = "Cancel";

  const commit = () => {
    const note = input.value.trim();
    row.remove();
    stopRunningCommand(toolId, note);
  };
  const dismiss = () => row.remove();

  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); commit(); }
    else if (e.key === "Escape") { e.preventDefault(); dismiss(); }
  });
  confirm.addEventListener("click", commit);
  cancel.addEventListener("click", dismiss);

  row.append(input, confirm, cancel);
  body.appendChild(row);
  input.focus();
}

customElements.define("pu-tool", ToolBlock);
