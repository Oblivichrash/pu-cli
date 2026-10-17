// SPDX-License-Identifier: GPL-3.0-only
// The panel holds what is read rather than chosen: where the last reply ran, how much
// context it ran against, and which tools the agent can reach.

"use strict";

const BACKEND_FIELDS = { type: "kb-type", model: "kb-model", host: "kb-host" };

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text || "—";
}

function setBackendLabel(backendType, model, host) {
  if (backendType) PU.backendTypeShown = backendType;
  setText(BACKEND_FIELDS.type, PU.backendTypeShown);
  if (model) setText(BACKEND_FIELDS.model, model);
  if (host) setText(BACKEND_FIELDS.host, host);
}

// No window is shown beside the counts: no gateway here reports one, and a guess reads as fact.
function handleUsage(payload) {
  const el = document.getElementById("context-usage");
  const fill = document.getElementById("context-fill");
  if (!el || !payload) return;
  const prompt = Number(payload.prompt_tokens);
  const completion = Number(payload.completion_tokens);
  if (!Number.isFinite(prompt)) {
    el.textContent = "";
    if (fill) fill.style.width = "0";
    return;
  }
  el.textContent =
      `${prompt.toLocaleString()} tokens` +
      (Number.isFinite(completion) ? `  ·  +${completion.toLocaleString()}` : "");
  if (!fill) return;
  // The bar needs a known window; without one the number stands alone.
  const window = contextWindowFor(PU.backendTypeShown);
  if (!window) {
    fill.style.width = "0";
    return;
  }
  const ratio = Math.min(prompt / window, 1);
  fill.style.width = (ratio * 100).toFixed(1) + "%";
  fill.className = ratio > 0.85 ? "high" : (ratio > 0.6 ? "warn" : "");
}

function renderToolList(tools) {
  const list = document.getElementById("panel-tools");
  if (!list) return;
  list.replaceChildren();
  if (!Array.isArray(tools) || tools.length === 0) {
    const li = document.createElement("li");
    li.textContent = "—";
    li.style.color = "var(--ink-light)";
    list.appendChild(li);
    return;
  }
  for (const name of tools) {
    const li = document.createElement("li");
    li.textContent = name;
    if (name.startsWith("mcp.")) {
      const src = document.createElement("span");
      src.className = "tool-src";
      src.textContent = "mcp";
      li.appendChild(src);
    }
    list.appendChild(li);
  }
}
