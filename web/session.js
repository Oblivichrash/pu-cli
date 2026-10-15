// SPDX-License-Identifier: GPL-3.0-only
// Everything the page learns from the server: the session it is attached to, the replies
// it replays into the transcript, and the two controls that change it.

"use strict";

// The store is the authority on the chain's length, so it is read back after every run.
async function refreshChainLength() {
  try {
    const history = await fetchJson("/api/history");
    if (Array.isArray(history)) PU.chainLength = history.length;
  } catch (_) {}
}

async function loadHistory() {
  try {
    const history = await fetchJson("/api/history");
    if (!Array.isArray(history)) return;
    // A tool-using turn is several messages; drawing each alone would show another chat.
    for (const turn of groupHistory(history)) {
      if (turn.role === "system") {
        createSystemMessage(turn.text);
      } else if (turn.role === "user") {
        createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: turn.text }], turn.turn,
                      turn.text);
      } else {
        createMessage("assistant", turn.blocks);
      }
    }
    PU.chainLength = history.length;
  } catch (_) {}
}

// Only the server tells a session level from the agent's, so the control shows what it reports.
function renderThinkingControl(level, override) {
  const select = PU.composer.thinkingSelect;
  const sessionLevel = override && override !== "default" ? override : null;
  select.value = sessionLevel || "auto";
  select.title = "Thinking: " + level +
    (sessionLevel ? " (set for this session)" : " (from the agent's configuration)");
}

async function loadSession() {
  try {
    const data = await fetchJson("/api/session");
    if (!data.success) return;
    setBackendLabel(data.backend_type, data.backend_model, data.backend_host);
    renderThinkingControl(data.thinking || "default", data.thinking_override || null);
    if (data.agent_name) PU.composer.agentSelect.value = data.agent_name;
    renderToolList(data.tools);
  } catch (_) {}
}

async function setThinkingLevel(level) {
  try {
    const data = await postJson("/api/thinking", { level });
    if (!data.success) {
      createSystemMessage("Thinking could not be set: " + (data.error || "unknown error"));
      return;
    }
    renderThinkingControl(data.thinking, data.thinking_override || null);
  } catch (e) {
    createSystemMessage("Thinking could not be set: " + e.message);
  }
}

function renderAgentOptions(agents) {
  const select = PU.composer.agentSelect;
  select.replaceChildren();
  if (agents && agents.length > 0) {
    for (const agent of agents) {
      const opt = document.createElement("option");
      opt.value = agent.name;
      opt.textContent = agent.name + (agent.description ? " (" + agent.description + ")" : "");
      select.appendChild(opt);
    }
    return;
  }
  const opt = document.createElement("option");
  opt.value = "";
  opt.textContent = "No agents available";
  select.appendChild(opt);
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
    input.value = text;
    input.focus();
    input.style.height = "auto";
    createSystemMessage("Editing turn " + turn +
                        ". Sending replaces it; the turn it replaced is not kept.");
  } catch (e) {
    createSystemMessage("Could not step back: " + e.message);
  }
}
