// SPDX-License-Identifier: GPL-3.0-only
// What is read rather than chosen: where the last reply ran, how much context it ran against,
// which tools the agent can reach, and the status bar that repeats the session's own facts.

"use strict";

const BACKEND_FIELDS = { type: "gw-type", host: "gw-host" };

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text || "—";
}

// The gateway and the model sit in selects, so the label is written into the control's value:
// textContent on a select would read as its options rather than as the one chosen.
function setSelectValue(id, value) {
  const el = document.getElementById(id);
  if (el) el.value = value || "";
  return el;
}

// The address is a field the reader types in, so drawing it is not the same act as drawing a menu:
// a value arriving while the reader has the field in hand must not overwrite what they are typing.
// Only an unfocused field takes the drawn value.
function setHostValue(host) {
  const el = document.getElementById(BACKEND_FIELDS.host);
  if (!el) return;
  if (document.activeElement !== el) el.value = host || "";
}

// The form holds what the build carries; a label says which one is in force and never grows the
// list. The model is no longer drawn into a control here -- it is chosen from the bar -- so this
// only has to keep the reading of it in step with the server's answer.
function setBackendLabel(gateway, model, host) {
  if (gateway) PU.gatewayShown = gateway;
  setSelectValue(BACKEND_FIELDS.type, PU.gatewayShown);
  // An absent host is a drawn fact, not a skipped one: the field shows the gateway's default when
  // the server answers with no address, so the reader can see what would be written if they typed
  // over it. `undefined` is a caller with nothing to say; `""` is the server saying so.
  if (host !== undefined) {
    setHostValue(host);
    PU.backendHostShown = host || "";
  }
  if (model) PU.backendModelShown = model;
  renderStatusBar();
  // The window is filed under the model, so a label naming a different one changes which window
  // applies. A gateway that picked the model (`done` names it) has to redraw the bar here.
  renderContextBar();
}

// "gateway / model", the two facts a reader checks between turns. Kept as one string for the
// places that want them together -- a system message, a tooltip -- but the bar draws the two
// halves into their own elements so each can be pressed on its own.
function backendSummary() {
  const gateway = PU.gatewayShown || "";
  const model = PU.backendModelShown || "";
  return gateway && model ? gateway + " / " + model : (gateway || model);
}

// The bar repeats what the dialog holds, in one line.
function renderStatusBar() {
  // The gateway and the model are drawn apart because they are two choices, not one: a reader
  // who wants the other model has no reason to be shown the address and the temperature, and a
  // shared target would have to guess which half of "openai / stub-model" was pressed. Each takes
  // a dash rather than being emptied when there is nothing to say, so the bar keeps its shape
  // through the first render.
  setText("status-gateway", PU.gatewayShown || "");
  setText("status-model", PU.backendModelShown || "");
  // The agent's own name is stated in the dialog rather than chosen, so it is read once per
  // render from what the session reported.
  setText("config-agent-name", PU.agentShown || "");

  // What the server last said about thinking, rather than what a control holds: a setting an
  // older page does not draw is still a fact about the session.
  const thinking = settingValue("thinking");
  setText("status-thinking", thinking === null ? "—" : String(thinking));

  // The usage item states its own figures -- the count and the hairline -- so the bar only has
  // to keep them in step with whatever landed last.
  renderContextBar();
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

// Puts the turn's cost beside the reply it paid for. A reload draws this line from the stored turn
// through `createMessage`; a live turn never went through that path, so the same node is added here
// by the same function and the two cannot say different things.
//
// The bubble takes it only once: `usage` is sent once per turn, but a resync can replay it after a
// reconnect has already redrawn the turn from the store.
function attachUsageToOpenReply(payload) {
  if (!Number.isFinite(Number(payload.prompt_tokens))) return;
  const el = PU.currentAssistantEl;
  if (!el) return;
  el.querySelectorAll(".msg-usage").forEach((old) => old.remove());
  // The cost is the last line inside the bubble, and a live turn has none until this frame makes
  // one. A bubble that already has the line from the store keeps it where it is rather than growing
  // a second one.
  const foot = el.querySelector(".msg-bubble > .msg-foot");
  if (foot) foot.appendChild(usageLineNode(payload));
  else {
    const line = document.createElement("div");
    line.className = "msg-foot";
    line.appendChild(usageLineNode(payload));
    (el.querySelector(".msg-bubble") || el).appendChild(line);
  }
}

// Draws one turn's usage on the status bar, and the conversation's total into the same tooltip. The
// socket sends both together because the turn that just ended is part of the total, so a figure
// drawn without its total would answer a question the reader has already asked.
function showUsage(payload) {
  if (!payload) return;
  if (payload.total) renderUsageTotal(payload.total);
  // The frame arrives before the `done` that closes the bubble, so the reply on screen is still the
  // live one; without this the figure only appeared on reload, where the stored turn carries it
  // into the replayed message, and a live turn read as if it had cost nothing.
  attachUsageToOpenReply(payload);
  const prompt = Number(payload.prompt_tokens);
  const completion = Number(payload.completion_tokens);
  const cached = Number(payload.cached_tokens);
  if (!Number.isFinite(prompt)) {
    PU.lastTurnPrompt = null;
    PU.lastTurnLine = "";
    renderStatusBar();
    renderContextBar();
    return;
  }
  // "12,345 tokens  ·  40% cached  ·  +312", kept so the tooltip repeats it rather than spelling it
  // out again and risking a different answer.
  PU.lastTurnLine =
      tokenLine(prompt, cached) +
      (Number.isFinite(completion) ? `  ·  +${completion.toLocaleString()}` : "");
  PU.contextShown = prompt.toLocaleString() + " tok";
  PU.lastTurnPrompt = prompt;
  renderStatusBar();
  renderContextBar();
}

// The fill is drawn from the two facts it needs rather than from whichever payload landed last: the
// counts arrive with a turn and the window arrives with the session, and a reload can deliver them
// in either order. Both are on `PU`, so either arrival can redraw.
function renderContextBar() {
  const fill = document.getElementById("usage-fill");
  const barEl = document.getElementById("status-usage");
  if (!fill || !barEl) return;
  const prompt = PU.lastTurnPrompt;
  const window = contextWindowFor(PU.gatewayShown, PU.backendModelShown);

  if (!Number.isFinite(prompt)) {
    // Nothing has been spent yet, so there is no ratio to draw and no colour to give it.
    setText("usage-text", "—");
    fill.style.width = "0";
    fill.className = "";
    barEl.className = "status-item usage";
    barEl.title = "How much context the last reply ran against";
    return;
  }

  // The count is always printed, since it is the honest half; the window is only shown when the
  // model states one, which is what turns the figure into a proportion.
  setText("usage-text", window
      ? `${prompt.toLocaleString()} / ${window.toLocaleString()}`
      : `${prompt.toLocaleString()} tok`);

  if (!window) {
    // No window: the count stands alone, which is what every gateway but the ones that state a
    // size leaves it. The hairline stays empty, since there is nothing to be a fraction of.
    fill.style.width = "0";
    fill.className = "";
    barEl.className = "status-item usage";
    renderUsageTitle();
    return;
  }

  const ratio = Math.min(prompt / window, 1);
  fill.style.width = (ratio * 100).toFixed(1) + "%";
  const level = ratio > 0.85 ? "high" : (ratio > 0.6 ? "warn" : "");
  fill.className = level;
  barEl.className = "status-item usage" + (level ? " " + level : "");
  renderUsageTitle();
}

// The tooltip is where every figure about the turn lives: what this turn cost, how much of the
// window it filled, what the conversation has spent. It is rebuilt whenever a figure arrives, since
// either arrival can be the last.
function renderUsageTitle() {
  const barEl = document.getElementById("status-usage");
  if (!barEl) return;
  const prompt = PU.lastTurnPrompt;
  if (!Number.isFinite(prompt)) return;
  const window = contextWindowFor(PU.gatewayShown, PU.backendModelShown);
  const lines = [PU.lastTurnLine || `${prompt.toLocaleString()} tokens`];
  if (window) {
    lines.push(`${Math.round(Math.min(prompt / window, 1) * 100)}% of ${window.toLocaleString()} tokens`);
  } else {
    lines.push("this model reports no window to measure it against");
  }
  const total = PU.usageTotal;
  if (total) {
    const sum = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);
    lines.push("");
    lines.push(
        `${sum(total.turns)} turn${sum(total.turns) === 1 ? "" : "s"}  ·  ` +
        `${sum(total.prompt_tokens).toLocaleString()} prompt  ·  ` +
        `${sum(total.completion_tokens).toLocaleString()} completion`);
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
  }
  barEl.title = lines.join("\n");
}

// The figures go into the usage tooltip; `PU.usageTotal` is what the next rebuild reads.
function renderUsageTotal(total) {
  PU.usageTotal = total && Number.isFinite(Number(total.turns)) ? total : null;
  renderUsageTitle();
}

function renderToolList(tools) {
  const list = document.getElementById("panel-tools");
  if (!list) return;
  list.replaceChildren();

  // The count is what the bar can hold; the names are what the dialog is for.
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
