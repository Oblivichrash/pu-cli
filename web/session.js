// SPDX-License-Identifier: GPL-3.0-only
// Everything the page learns from the server: the session it is attached to, the replies
// it replays into the transcript, and the two controls that change it.

"use strict";

// The store is the authority on the chain's length, so it is read back after every run. The
// spend figure needs no part in this: a finished turn carries its own total, so the frame
// updates it without a second ask.
async function refreshChainLength() {
  try {
    const history = (await fetchJson("/api/history")).messages;
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
    const history = (await fetchJson("/api/history")).messages;
    if (!Array.isArray(history)) return;
    // A tool-using turn is several messages; drawing each alone would show another chat.
    for (const turn of groupHistory(history)) {
      if (turn.role === "system") {
        createSystemMessage(turn.text);
      } else if (turn.role === "user") {
        createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: turn.text }], turn.turn,
                      turn.text);
      } else {
        createMessage("assistant", turn.blocks, undefined, undefined, turn.usage);
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
    // The server that answered says what it is called and which port it answered on. That is the
    // page's own identity, so a switch ends here: the label is corrected from the server the page
    // actually reached rather than from the entry it asked for.
    if (data.server) setSelf({ name: data.server.name, port: data.server.port });
    renderServers();
    setBackendLabel(data.gateway, data.backend_model, data.backend_host);
    // The window belongs to the model rather than to the gateway, so it is filed under both and
    // drawn here: a trial of a turn may already have been drawn against no window, and a switch
    // to a model that states one has to fill the bar without waiting for the next turn.
    setContextWindow(data.gateway, data.backend_model, data.context_window);
    renderContextBar();
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

// Editing a turn of your own happens in the bubble that holds it: the words become a field, the
// transcript is not rebuilt and the view does not jump to the composer. What a save cannot do is
// keep what came after -- every later turn was an answer to this text -- so the session is
// stepped back to just before it, and the field stays open in place of a reply that is no longer
// coming. Sending then replaces the turn.
//
// Only the turn being edited is touched. A step back before turn N drops everything from N on,
// and those bubbles are dropped here too, so the page and the store agree without a re-read.
//
// `fromButton` is set by the Edit control, whose own click would otherwise be read as a click
// on a button inside the bubble and ignored: that guard exists to keep a press on a link or a
// fold from opening the field, and the control says plainly that it is the way in.
function beginEdit(el, event, fromButton) {
  // Selecting text to copy it also clicks. A drag that leaves a selection is not a request to
  // edit, and neither is a click that landed on a fold or a link inside the bubble.
  if (!fromButton && event &&
      event.target.closest("a, button, .block-tool, .block-subtask, .block-thinking")) {
    return;
  }
  const selection = window.getSelection();
  if (selection && selection.toString().trim()) return;
  if (el.dataset.editing === "1") return;
  if (PU.isStreaming) {
    createSystemMessage("Cannot edit while a reply is streaming.");
    return;
  }
  const turn = Number(el.dataset.turn);
  const original = el.dataset.text || "";
  el.dataset.editing = "1";
  el.classList.add("editing");
  el.title = "";
  const container = el.querySelector(".blocks-container");
  const field = document.createElement("textarea");
  field.className = "edit-field";
  field.value = original;
  container.replaceChildren(field);
  // The bar under the field is where everything a reader may do with this message is offered at
  // once: copy and edit, then the two that end the edit. They used to sit under the bubble at
  // rest, which put a row of marks under every turn whether or not anyone meant to act on it.
  // Copy stays here beside Save because the field holds the text as it is *now*: a reader who
  // has just changed a sentence often wants the new words somewhere else, and taking them from
  // the field is the only reading of "copy" that matches what is on screen.
  const actions = document.createElement("div");
  actions.className = "edit-actions";
  actions.appendChild(messageActions(el, "user", turn, blocksOf(el)));

  const save = document.createElement("button");
  save.type = "button";
  save.className = "edit-save";
  save.textContent = "Save";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.className = "edit-cancel";
  cancel.textContent = "Cancel";
  actions.append(save, cancel);
  // Inside the bubble, with the field: these belong to the sentence being changed, not to the
  // line under it that says what the turn cost.
  (el.querySelector(".msg-bubble") || el).appendChild(actions);

  // `value` is what the bubble says once the field is gone: `null` is a cancelled edit, which
  // goes back to what it said before. Either way the sentence lands where the field was, so the
  // reader is never left looking at a form for a message that is finished.
  const close = (value) => {
    delete el.dataset.editing;
    el.classList.remove("editing");
    el.title = "Click to edit this message";
    actions.remove();
    const text = value === null ? (el.dataset.text || "") : value;
    // The bar offered a copy of the words as they were when it was built, and the field has
    // since been edited past them: what the message carries now is what a copy should take.
    el._puBlocks = [{ type: BLOCK_TYPES.TEXT, content: text }];
    renderBlocks([{ type: BLOCK_TYPES.TEXT, content: text }], container);
  };

  // The field is the only thing in the bubble, so it grows with what is in it.
  const fit = () => {
    field.style.height = "auto";
    field.style.height = field.scrollHeight + "px";
  };
  field.addEventListener("input", fit);
  requestAnimationFrame(() => {
    fit();
    field.focus();
    field.setSelectionRange(field.value.length, field.value.length);
  });

  // Escape leaves the text as it was; Enter with the modifier that means a newline is not a save.
  field.addEventListener("keydown", (keyEvent) => {
    if (keyEvent.key === "Escape") {
      keyEvent.preventDefault();
      close(null);
    } else if (keyEvent.key === "Enter" && !keyEvent.shiftKey && !keyEvent.isComposing) {
      keyEvent.preventDefault();
      save.click();
    }
  });
  cancel.addEventListener("click", (clickEvent) => {
    clickEvent.stopPropagation();
    close(null);
  });
  save.addEventListener("click", async (clickEvent) => {
    clickEvent.stopPropagation();
    const text = field.value.trim();
    if (!text) return;
    if (text === original) {
      close(null);
      return;
    }
    // Everything after this bubble is the conversation the store is about to lose -- its own
    // words if this turn is re-said, and the replies to turns that followed. The step back
    // happens before the text is offered for sending, so the composer's turn number and the
    // store's chain agree by the time a later send is made.
    //
    // The bubble being edited is the one that stays; it is this turn re-said rather than a new
    // one, so the sweep starts after it and never takes it.
    const dropped = [];
    for (let node = el.nextElementSibling; node; node = node.nextElementSibling) {
      dropped.push(node);
    }
    try {
      const data = await postJson("/api/rewind", { turn });
      if (!data.success) {
        createSystemMessage("Could not step back: " + (data.error || "unknown error"));
        return;
      }
    } catch (e) {
      createSystemMessage("Could not step back: " + e.message);
      return;
    }
    for (const node of dropped) node.remove();
    // A reply still being written belonged to a turn just dropped: it must not keep drawing
    // into a conversation that has been stepped back under it.
    if (PU.currentAssistantEl && !PU.composer.messagesEl.contains(PU.currentAssistantEl)) {
      PU.currentAssistantEl = null;
      PU.currentAssistantBlocks = [];
    }
    el.dataset.text = text;
    close(text);
    await refreshChainLength();
    offerResend(el, text);
  });
}

// A saved edit is a turn of yours that has been said but not asked: the store was stepped back
// to before it, so it is once again the newest thing you said. It keeps its place in the
// transcript and grows one control, which is the only thing on the page that sends it.
function offerResend(el, text) {
  el.querySelectorAll(".edit-resend").forEach((old) => old.remove());
  const bar = document.createElement("div");
  bar.className = "edit-resend";
  const note = document.createElement("span");
  note.className = "edit-resend-note";
  note.textContent = "Edited. The turns after this one were dropped.";
  const send = document.createElement("button");
  send.type = "button";
  send.className = "edit-resend-btn";
  send.textContent = "Send";
  send.addEventListener("click", (event) => {
    event.stopPropagation();
    bar.remove();
    if (!sendText(text, el)) {
      // Nothing was sent, so the offer has to stay: losing it would leave a turn the store no
      // longer holds and no way to ask it again except by retyping it.
      offerResend(el, text);
      return;
    }
  });
  bar.append(note, send);
  // The offer to re-send belongs to the turn that was just changed, so it stays inside the
  // bubble rather than joining the footing, which describes a message that is settled.
  (el.querySelector(".msg-bubble") || el).appendChild(bar);
}
