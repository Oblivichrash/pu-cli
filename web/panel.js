// SPDX-License-Identifier: GPL-3.0-only
// What is read rather than chosen: where the last reply ran, how much context it ran
// against, which tools the agent can reach, and the status bar that repeats the session's
// own facts so a folded panel costs no state.

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
  if (model) PU.backendModelShown = model;
  if (host) PU.backendHostShown = host;
  renderStatusBar();
}

// The bar repeats what the panel holds, in one line, because the panel folds and the
// session's own facts do not change when it does.
function renderStatusBar() {
  const agent = PU.composer.agentSelect ? PU.composer.agentSelect.value : "";
  setText("status-agent", agent);
  // The bar's copy is cut at 220px, so it carries the same tooltip the panel's control does.
  const agentEl = document.getElementById("status-agent");
  if (agentEl) {
    const description = (PU.agentDescriptions || {})[agent];
    agentEl.title = description || "The agent that answers";
  }

  const thinking = PU.composer.thinkingSelect ? PU.composer.thinkingSelect.value : "";
  setText("status-thinking", thinking === "auto" ? "auto" : thinking);

  const gateway = PU.backendTypeShown;
  const model = PU.backendModelShown;
  setText("status-backend", gateway && model ? `${gateway} · ${model}` : (gateway || model));
  // The host is a fact about the configuration rather than about this turn, and it is long
  // enough to crowd the bar, so it rides where the rest of the address already does.
  const backendEl = document.getElementById("status-backend");
  if (backendEl) {
    backendEl.title = PU.backendHostShown
      ? `${gateway || "—"} · ${model || "—"}\n${PU.backendHostShown}`
      : "The gateway and model";
  }

  setText("status-context", PU.contextShown || "—");
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
  PU.contextShown = prompt.toLocaleString() + " tok";
  renderStatusBar();
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

  // The bar has no room for a progress bar, so the number carries the same warning the
  // panel's fill does.
  const barEl = document.getElementById("status-context");
  if (barEl) {
    barEl.className = "status-item" + (ratio > 0.85 ? " high" : (ratio > 0.6 ? " warn" : ""));
    barEl.title = `Context the last reply ran against: ${Math.round(ratio * 100)}% of ${window.toLocaleString()}`;
  }
}

function renderToolList(tools) {
  const list = document.getElementById("panel-tools");
  if (!list) return;
  list.replaceChildren();

  // The count is what the bar can hold; the names are what the panel is for.
  const count = Array.isArray(tools) ? tools.length : 0;
  setText("status-tools", count === 0 ? "no tools" : `${count} tool${count === 1 ? "" : "s"}`);
  const toolsEl = document.getElementById("status-tools");
  if (toolsEl && count > 0) toolsEl.title = tools.join(", ");

  if (count === 0) {
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
