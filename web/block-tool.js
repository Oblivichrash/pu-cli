// SPDX-License-Identifier: GPL-3.0-only
// A tool call's card: what was asked, what came back, and the one action that belongs to
// a command still running.

"use strict";

// Fields whose value says what a step is doing, tried before any other. `command` and
// `path` cover the built-in tools; the rest are common enough that naming them costs
// nothing, and a tool nobody listed falls through to the first string it carries.
const SUMMARY_FIELDS = ["command", "path", "query", "pattern", "url"];

const SUMMARY_MAX = 60;

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
  return count === 0 ? "" : count + " 参数";
}

function oneLine(text) {
  const flat = text.replace(/\s+/g, " ").trim();
  return flat.length > SUMMARY_MAX ? flat.slice(0, SUMMARY_MAX) + "…" : flat;
}

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

    this.nameEl = document.createElement("span");
    this.nameEl.className = "tool-name";
    this.summaryEl = document.createElement("span");
    this.summaryEl.className = "tool-summary";
    this.statusEl = document.createElement("span");
    this.statusEl.className = "tool-status";

    header.append(this.nameEl, this.summaryEl, this.statusEl);
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
    this.statusEl.textContent = this.#statusText(block);
    syncCollapsible(this, block);
    this.#fillBody(block);
  }

  toggle() {
    toggleSelf(this);
  }

  toggled() {
    return this._block.collapsed;
  }

  // A finished call that worked says so by being finished, so only the states a reader
  // cannot infer are named: still running, not answered, and stopped before it ran.
  #statusText(block) {
    if (block.status === "running") return "running…";
    if (block.status === "pending") return "no result";
    if (block.status === "interrupted") return "interrupted";
    return block.error ? "failed" : "";
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
