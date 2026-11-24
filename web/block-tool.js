// SPDX-License-Identifier: GPL-3.0-only
// A tool call's card: what was asked, what came back, and the one action that belongs to
// a command still running.

"use strict";

// Fields whose value says what a step is doing, tried before any other. `command` and
// `path` cover the built-in tools; the rest are common enough that naming them costs
// nothing, and a tool nobody listed falls through to the first string it carries.
const SUMMARY_FIELDS = ["command", "path", "query", "pattern", "url"];

// A one-line gist of the arguments, never the whole JSON: a long command is cut, and an
// argument set with no readable value is counted rather than spelled out.
function summarizeArgs(args) {
  if (!args || typeof args !== "object") return "";
  for (const key of SUMMARY_FIELDS) {
    const value = args[key];
    if (typeof value === "string" && value) return value;
  }
  for (const value of Object.values(args)) {
    if (typeof value === "string" && value) return value;
  }
  const count = Object.keys(args).length;
  return count === 0 ? "" : count + " args";
}

// A page's account of itself, in the one line a collapsed card has room for: which lines of the
// command's output these are, and how much of it the command printed. Empty for a result that is
// not a page -- the whole output is in hand, and saying so would be noise on every ordinary call.
function pageSummary(page) {
  if (!page || typeof page !== "object") return "";
  const lines = page.lines;
  const stored = page.stored;
  if (!lines || !stored) return "";
  const shown = `${lines.first}\u2013${lines.last}`;
  const of = Number(lines.of) || 0;
  const kept = Number(stored.kept) || 0;
  const dropped = Number(stored.dropped) || 0;
  if (dropped === 0 && of === kept) return `lines ${shown} of ${of}`;
  // Dropped lines are only interesting where they came from: a tail that starts mid-output is
  // a different reading from a head that stops early, and `kept_from` is what says which.
  const where = stored.kept_from === "the end" ? "first " : "last ";
  return `lines ${shown} of ${of}, ${where}${kept} kept`;
}

class ToolBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-tool";

    const header = document.createElement("div");

    this.nameEl = document.createElement("span");
    this.nameEl.className = "tool-name";
    this.summaryEl = document.createElement("span");
    this.summaryEl.className = "tool-summary";
    this.statusEl = document.createElement("span");
    this.statusEl.className = "tool-status";
    this.pageEl = document.createElement("span");
    this.pageEl.className = "tool-page";

    header.append(this.nameEl, this.summaryEl, this.statusEl, this.pageEl);
    buildCollapsible(this, header);
    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    this.dataset.toolId = block.id || "";
    const summary = oneLine(summarizeArgs(block.args));
    this.nameEl.textContent = block.name || "unknown";
    this.summaryEl.textContent = summary;
    this.summaryEl.hidden = !summary;
    this.statusEl.textContent = blockStatusText(block);
    const page = pageSummary(block.page);
    this.pageEl.textContent = page;
    this.pageEl.hidden = !page;
    syncCollapsible(this, block);
    this.#fillBody(block);
  }

  toggle() {
    toggleSelf(this);
  }

  #fillBody(block) {
    this.body.replaceChildren();
    // Tool output is a command's raw product, so it is set as text: its markup is data.
    this.body.appendChild(
        labelledPre("tool-args", "Arguments", JSON.stringify(block.args ?? null, null, 2), ""));
    if (block.status === "running" || block.status === "pending") return;
    this.body.appendChild(labelledPre(
        "tool-result",
        block.error ? "Error" : "Output",
        block.error || block.output || "(no output)",
        block.error ? "tool-error" : ""));
  }
}

customElements.define("pu-tool", ToolBlock);
