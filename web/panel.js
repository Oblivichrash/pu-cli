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
  // The window is filed under the model, so a label naming a different one changes which window
  // applies. A gateway that picked the model (`done` names it) has to redraw the bar here.
  renderContextBar();
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

  // What the server last said about thinking, rather than what a control holds: a setting an
  // older page does not draw is still a fact about the session.
  const thinking = settingValue("thinking");
  setText("status-thinking", thinking === null ? "—" : String(thinking));

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

// "1,234 tok · 40% cached", or the count alone where the gateway said nothing about a cache.
function tokenLine(prompt, cached) {
  if (!Number.isFinite(prompt)) return "";
  let line = `${prompt.toLocaleString()} tokens`;
  if (Number.isFinite(cached) && prompt > 0 && cached > 0) {
    line += `  ·  ${Math.round((cached / prompt) * 100)}% cached`;
  }
  return line;
}

// Draws one turn's usage in the panel and the bar, and the conversation's total under it. The
// socket sends both together because the turn that just ended is part of the total, so a frame
// drawn without its total would answer a question the reader has already asked.
function showUsage(payload) {
  const el = document.getElementById("context-usage");
  const fill = document.getElementById("context-fill");
  if (!el || !payload) return;
  if (payload.total) renderUsageTotal(payload.total);
  const prompt = Number(payload.prompt_tokens);
  const completion = Number(payload.completion_tokens);
  const cached = Number(payload.cached_tokens);
  if (!Number.isFinite(prompt)) {
    el.textContent = "";
    PU.lastTurnPrompt = null;
    renderContextBar();
    return;
  }
  el.textContent =
      tokenLine(prompt, cached) +
      (Number.isFinite(completion) ? `  ·  +${completion.toLocaleString()}` : "");
  PU.contextShown = prompt.toLocaleString() + " tok";
  PU.lastTurnPrompt = prompt;
  renderStatusBar();
  renderContextBar();
}

// The fill, drawn from the two facts it needs rather than from whichever payload landed last:
// the counts arrive with a turn and the window arrives with the session, and a reload can
// deliver them in either order. Both are on `PU`, so either arrival can redraw.
function renderContextBar() {
  const fill = document.getElementById("context-fill");
  const prompt = PU.lastTurnPrompt;
  const window = contextWindowFor(PU.backendTypeShown, PU.backendModelShown);

  // The bar has no room for a progress bar, so the status line carries the same warning.
  const barEl = document.getElementById("status-context");
  if (!fill) return;

  if (!Number.isFinite(prompt) || !window) {
    // No window: the count stands alone, which is what every gateway but the ones that state
    // a size leaves it. Said in the tooltip rather than left as a bare dash.
    fill.style.width = "0";
    fill.className = "";
    if (barEl) {
      barEl.className = "status-item";
      barEl.title = window
        ? "Context the last reply ran against"
        : "Context the last reply ran against; this model reports no window to measure it against";
    }
    return;
  }

  const ratio = Math.min(prompt / window, 1);
  fill.style.width = (ratio * 100).toFixed(1) + "%";
  fill.className = ratio > 0.85 ? "high" : (ratio > 0.6 ? "warn" : "");
  if (barEl) {
    barEl.className = "status-item" + (ratio > 0.85 ? " high" : (ratio > 0.6 ? " warn" : ""));
    barEl.title = `Context the last reply ran against: ${Math.round(ratio * 100)}% of ${window.toLocaleString()}`;
  }
}

// What the conversation has spent. The counts are sums; a price is drawn only when every turn
  // that reported one agreed on the unit, which is what the server already decided.
function renderUsageTotal(total) {
  const host = document.getElementById("usage-total");
  if (!host) return;
  PU.usageTotal = total || null;
  if (!total || !Number.isFinite(Number(total.turns))) {
    host.textContent = "";
    return;
  }
  const sum = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);
  const lines = [
    `${sum(total.turns)} turn${sum(total.turns) === 1 ? "" : "s"}  ·  ` +
        `${sum(total.prompt_tokens).toLocaleString()} prompt  ·  ` +
        `${sum(total.completion_tokens).toLocaleString()} completion`,
  ];
  if (sum(total.cached_tokens) > 0) {
    lines.push(`${sum(total.cached_tokens).toLocaleString()} tokens came from the cache`);
  }
  if (total.cost && Number.isFinite(Number(total.cost.amount))) {
    lines.push(`${Number(total.cost.amount)} ${total.cost.unit || ""}`.trim());
  } else if (sum(total.priced_turns) > 0) {
    // Some turns were priced and some were not, or two units were mixed: a total would lie.
    lines.push(`price not summed (${sum(total.priced_turns)} of ${sum(total.turns)} turns priced)`);
  } else {
    lines.push("no gateway here reported a price");
  }
  host.replaceChildren();
  for (const line of lines) {
    const row = document.createElement("div");
    row.textContent = line;
    host.appendChild(row);
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
