// SPDX-License-Identifier: GPL-3.0-only
// Everything the page learns from the server: the session it is attached to, the replies
// it replays into the transcript, and the two controls that change it.

"use strict";

// The store is the authority on the chain's length, so it is read back after every run. The
// spend figure needs no part in this: a finished turn carries its own total, so the frame
// updates it without a second ask.
async function refreshChainLength() {
  try {
    const history = await fetchJson("/api/history");
    if (Array.isArray(history)) PU.chainLength = history.length;
  } catch (_) {}
}

async function loadHistory() {
  // Appending a second copy of the chain into a list another run is filling is what drew
  // every turn twice, so one run at a time. The socket's hold on incoming frames is not
  // this function's to raise or lower: see beginLoad/endLoad in ws.js.
  if (PU.loadHistBusy) return;
  PU.loadHistBusy = true;
  try {
    const history = await fetchJson("/api/history");
    if (!Array.isArray(history)) return;
    // A tool-using turn is several messages; drawing each alone would show another chat.
    for (const turn of groupHistory(history)) {
      if (turn.role === "system") {
        createSystemMessage(turn.text);
      } else if (turn.role === "user") {
        createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: turn.text }], turn.turn,
                      turn.text, turn.fromPeer);
      } else {
        createMessage("assistant", turn.blocks, undefined, undefined, undefined, turn.usage);
      }
    }
    PU.chainLength = history.length;
    // The bar and the panel describe the last reply, and a reload has no stream to learn it
    // from: the stored turn carries the same figure the socket would have sent.
    const replies = groupHistory(history).filter((turn) => turn.role === "assistant" && turn.usage);
    if (replies.length > 0) showUsage(replies[replies.length - 1].usage);
  } catch (_) {
  } finally {
    PU.loadHistBusy = false;
  }
}

// --- Settings ---------------------------------------------------------------------------
//
// Every setting the server knows arrives in one list: `{name, type, value, source, agent_value}`.
// `source` says whether the value in force is this session's choice or the agent's own. One
// renderer draws all of them, so a setting added on the server needs no change here.

function settingEntry(name) {
  return (PU.composer.settings || []).find((entry) => entry.name === name) || null;
}

function settingValue(name) {
  const entry = settingEntry(name);
  return entry ? entry.value : null;
}

// "high (this session)" or "0.7 (from the agent)". The parenthetical is the part that says
// whether a reset would change anything.
function describeSetting(entry) {
  const shown = entry.value === null || entry.value === undefined ? "—" : String(entry.value);
  return entry.source === "session" ? `${shown} (this session)` : `${shown} (from the agent)`;
}

function settingControl(entry) {
  let control;
  if (entry.type === "enum" && entry.name === "thinking") {
    // The levels are a closed set the server validates; the list is the page's copy of it.
    control = document.createElement("select");
    for (const level of ["auto", "none", "low", "medium", "high"]) {
      const option = document.createElement("option");
      option.value = level;
      option.textContent = level;
      control.appendChild(option);
    }
    control.value = entry.value === null || entry.value === undefined ? "auto" : String(entry.value);
  } else {
    control = document.createElement("input");
    control.type = "number";
    control.step = entry.type === "float" ? "0.1" : "1";
    control.value = entry.value === null || entry.value === undefined ? "" : String(entry.value);
    // An empty box means "the agent's number", which is what clearing the override does.
    control.placeholder = entry.agent_value === null || entry.agent_value === undefined
        ? "none" : String(entry.agent_value);
  }
  control.dataset.setting = entry.name;
  control.title = describeSetting(entry);
  control.disabled = PU.isStreaming;
  control.addEventListener("change", () => setSetting(entry.name, control));
  return control;
}

function renderSettings(settings) {
  PU.composer.settings = Array.isArray(settings) ? settings : [];
  const host = PU.composer.settingsRows;
  if (!host) return;
  host.replaceChildren();
  for (const entry of PU.composer.settings) {
    const row = document.createElement("label");
    row.className = "field";
    const label = document.createElement("span");
    label.textContent = entry.name.replace(/_/g, " ");
    row.appendChild(label);
    row.appendChild(settingControl(entry));
    host.appendChild(row);
  }
  renderStatusBar();
}

async function setSetting(name, control) {
  // An empty number box is a cleared override, not a zero: the server reads null that way and
  // refuses 0 for a budget, which would stop the turn at its first call.
  let value = control.value;
  if (control.type === "number") {
    value = control.value.trim() === "" ? null : Number(control.value);
  }
  try {
    const data = await postJson("/api/settings", { name, value });
    if (!data.success) {
      createSystemMessage(`Setting ${name} was refused: ${data.error || "unknown error"}`);
      return;
    }
    renderSettings(data.settings);
    renderUsageTotal(data.usage_total);
  } catch (e) {
    createSystemMessage(`Setting ${name} could not be set: ${e.message}`);
  }
}

async function resetSettings() {
  try {
    const data = await postJson("/api/settings/reset", {});
    if (!data.success) {
      createSystemMessage(`Settings could not be reset: ${data.error || "unknown error"}`);
      return;
    }
    renderSettings(data.settings);
  } catch (e) {
    createSystemMessage(`Settings could not be reset: ${e.message}`);
  }
}

async function loadSession() {
  try {
    const data = await fetchJson("/api/session");
    if (!data.success) return;
    setBackendLabel(data.backend_type, data.backend_model, data.backend_host);
    renderSettings(data.settings);
    renderUsageTotal(data.usage_total);
    if (data.agent_name) PU.composer.agentSelect.value = data.agent_name;
    renderToolList(data.tools);
    // The tooltip follows the agent the server reports, not the one the list last drew.
    showAgentDescription();
  } catch (_) {}
}

// The option holds the name alone: a description beside it is longer than the column and
// pushed the name itself out of view. The description is kept as a title, which the browser
// shows on hover, and copied onto the closed control so it reads without opening the list.
function renderAgentOptions(agents) {
  const select = PU.composer.agentSelect;
  select.replaceChildren();
  PU.agentDescriptions = {};
  if (agents && agents.length > 0) {
    for (const agent of agents) {
      const opt = document.createElement("option");
      opt.value = agent.name;
      opt.textContent = agent.name;
      if (agent.description) {
        opt.title = agent.description;
        PU.agentDescriptions[agent.name] = agent.description;
      }
      select.appendChild(opt);
    }
    showAgentDescription();
    return;
  }
  const opt = document.createElement("option");
  opt.value = "";
  opt.textContent = "No agents available";
  select.appendChild(opt);
  showAgentDescription();
}

// The closed control carries the chosen agent's description, since a tooltip on an option
// is shown only while the list is open and not by every browser.
function showAgentDescription() {
  const select = PU.composer.agentSelect;
  if (!select) return;
  const description = (PU.agentDescriptions || {})[select.value];
  select.title = description || select.value || "";
  renderStatusBar();
}

async function switchAgent(name) {
  try {
    const data = await postJson("/api/agent/switch", { agent_name: name });
    if (!data.success) {
      createSystemMessage("Switch failed: " + (data.error || "unknown error"));
      return;
    }
    PU.composer.messagesEl.replaceChildren();
    await loadSession();
    await loadHistory();
    scrollToBottom();
  } catch (e) {
    createSystemMessage("Switch failed: " + e.message);
  }
}

async function loadAgents() {
  const select = PU.composer.agentSelect;
  try {
    const data = await fetchJson("/api/agents");
    if (PU.composer.agentChangeHandler) {
      select.removeEventListener("change", PU.composer.agentChangeHandler);
    }
    renderAgentOptions(data.agents);
    PU.composer.agentChangeHandler = () => {
      showAgentDescription();
      if (select.value) switchAgent(select.value);
    };
    select.addEventListener("change", PU.composer.agentChangeHandler);
  } catch (e) {
    createSystemMessage("Failed to load agents: " + e.message);
    console.error("loadAgents error:", e);
  }
}

// Steps the session back before `turn` and hands the text to the composer, so sending it
// again replaces that turn.
async function rewindToTurn(turn, text) {
  if (PU.isStreaming) {
    createSystemMessage("Cannot edit while a reply is streaming.");
    return;
  }
  const input = PU.composer.inputEl;
  try {
    const data = await postJson("/api/rewind", { turn });
    if (!data.success) {
      createSystemMessage("Could not step back: " + (data.error || "unknown error"));
      return;
    }
    PU.composer.messagesEl.replaceChildren();
    await loadHistory();
    scrollToBottom();
    input.value = text;
    input.focus();
    input.style.height = "auto";
    createSystemMessage("Editing turn " + turn +
                        ". Sending replaces it; the turn it replaced is not kept.");
  } catch (e) {
    createSystemMessage("Could not step back: " + e.message);
  }
}
