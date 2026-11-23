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

// The turn a bubble stands for. A user message carries its number on the element because the
// menu and the edit both need it; a number the page cannot read is no turn, and a rewind needs a
// real one. This is read from the element rather than from the view's position in the list: a
// transcript that has been stepped back holds bubbles the store no longer counts.
function turnOf(el) {
  const turn = Number(el && el.dataset ? el.dataset.turn : NaN);
  return Number.isFinite(turn) && turn > 0 ? turn : 0;
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
    // The bar describes the last reply, and a reload has no stream to learn it from: the stored
    // turn carries the same figure the socket would have sent.
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
    // The agent is fixed by its file and cannot be changed from this page, so its name is only
    // carried and printed: the dialog states it beside the things that can be changed.
    if (typeof data.agent_name === "string") PU.agentShown = data.agent_name;
    setBackendLabel(data.gateway, data.backend_model, data.backend_host);
    // The window belongs to the model rather than to the gateway, so it is filed under both and
    // drawn here: a trial of a turn may already have been drawn against no window, and a switch
    // to a model that states one has to fill the bar without waiting for the next turn.
    setContextWindow(data.gateway, data.backend_model, data.context_window);
    renderContextBar();
    renderSettings(data.settings);
    renderUsageTotal(data.usage_total);
    renderToolList(data.tools);
    renderStatusBar();
  } catch (_) {}
}

// Which backend answers is the part of this page a reader can still change: the agent is fixed
// by its file, but the gateway and the model under it are a choice. What one gateway serves is
// asked when its menu is opened, because the catalogue belongs to the gateway; one that cannot
// be asked answers with nothing and a reason, and the model in force is left as typed text.
async function loadModelsFor(gateway) {
  if (!gateway) return;
  const sent = gateway;
  if (PU.modelCache && PU.modelCache.get(sent)) return PU.modelCache.get(sent).models;
  let reply;
  try {
    reply = await fetchJson("/api/models?gateway=" + encodeURIComponent(sent));
  } catch (e) {
    console.error("loadModels error:", e);
    return null;
  }
  const entry = { models: reply.models || [], error: reply.error || "" };
  if (!PU.modelCache) PU.modelCache = new Map();
  if (!entry.error) PU.modelCache.set(sent, entry);
  if (PU.composer.gatewaySelect && sent === PU.composer.gatewaySelect.value) {
    renderModels(entry.models, PU.backendModelShown, entry.error);
  }
  return entry.models;
}

// What the agent speaks through now, and -- from its own address -- every gateway this build
// carries. The two are read together because the menu is drawn from both at once, and neither
// answer depends on the other.
async function loadBackends() {
  try {
    const [answer, catalogue] = await Promise.all([
      fetchJson("/api/backend"),
      fetchJson("/api/gateways"),
    ]);
    if (!answer.success) {
      setBackendError(answer.error || "The backend in force could not be read.");
      return;
    }
    // The answer carries the backend under its own key, beside `success`.
    const backend = answer.backend || {};
    const gateways = catalogue.success ? catalogue.gateways || [] : [];
    if (!catalogue.success) setBackendError(catalogue.error || "The gateway list could not be read.");
    // No catalogue is asked for here: the model in force is drawn as the one option, and the
    // gateway's own list arrives when the menu is opened.
    renderBackendChoices(backend.gateway, gateways, backend.model, null, "", backend.host);
  } catch (e) {
    console.error("loadBackends error:", e);
    setBackendError(String(e && e.message ? e.message : e));
  }
}

// Every gateway this build carries is on offer, not just the one in force: a menu holding the
// current name alone could not be used to leave it. Redrawn only when the list differs, so a
// reader choosing in an open dropdown keeps it.
function renderGateways(gateways, gateway) {
  const { gatewaySelect } = PU.composer;
  if (!gatewaySelect) return;
  const known = gateways || [];
  // A page whose gateway list could not be read still names the one in force, rather than
  // showing an empty menu that looks like a build with no gateways.
  if (known.length === 0 && PU.gatewayShown && gatewaySelect.options.length <= 1) return;
  const drawn = Array.from(gatewaySelect.options, (o) => o.value);
  if (known.length > 0 && (drawn.length !== known.length ||
      drawn.some((v, i) => v !== known[i]))) {
    gatewaySelect.textContent = "";
    for (const name of known) gatewaySelect.append(new Option(name, name));
  }
  if (gateway) PU.gatewayShown = gateway;
  if (PU.gatewayShown) gatewaySelect.value = PU.gatewayShown;
}

// The model menu holds what the gateway serves, asked when the menu is opened rather than once
// at start: a catalogue belongs to the gateway, and the one being looked at is the only one worth
// a request. A gateway that lists nothing -- no key, host down -- leaves the name as typed text,
// so a model in force is never lost to a list that could not be fetched.
function renderModels(models, current, error) {
  const { modelSelect, modelText } = PU.composer;
  if (!modelSelect || !modelText) return;
  const asked = Array.isArray(models);
  const list = asked ? models : [];
  const want = current || PU.backendModelShown || "";
  // The name in force is an option before it is a value: a select assigned a value it has no
  // option for shows nothing, and a menu missing the running model would read as one not in use.
  // The menu is emptied only when a full list arrives to refill it. A list fetched and empty is
  // not a list of nothing to choose from -- the gateway could not say what it serves -- so the
  // name becomes text the reader can type.
  const typed = asked && list.length === 0;
  modelSelect.hidden = typed;
  modelText.hidden = !typed;
  if (typed) {
    modelText.value = want;
  } else {
    if (list.length > 0) {
      modelSelect.textContent = "";
      if (want && !list.includes(want)) modelSelect.append(new Option(want, want));
      for (const name of list) modelSelect.append(new Option(name, name));
    } else if (want && !Array.from(modelSelect.options, (o) => o.value).includes(want)) {
      modelSelect.append(new Option(want, want));
    }
    modelSelect.value = want || (list[0] || "");
  }
  if (current) PU.backendModelShown = current;
  setBackendError(error || "");
}

// The gateway menu and the model menu are drawn from one answer: what this build carries, and the
// backend in force. The model is written here and not again by the label, so the dialog holds one
// story about which model runs.
function renderBackendChoices(gateway, gateways, model, models, modelError, host) {
  renderGateways(gateways, gateway);
  if (model) PU.backendModelShown = model;
  renderModels(models, model, modelError);
  setBackendLabel(gateway, null, host);
}

function setBackendError(message) {
  const el = document.getElementById("kb-error");
  if (!el) return;
  el.textContent = message || "";
  el.hidden = !message;
}

// A chosen backend is written back by the server, so the next start uses it too. The reply is
// the same shape the GET answers in, and is drawn the same way rather than assuming the choice
// took: a gateway may refuse a model, and the page should show what is now in force.
//
// The model is settled before the request: what one gateway serves is not what the next one
// serves, so a name carried over from the old gateway is replaced by one the new gateway lists.
// A gateway that lists nothing -- no key, host down -- keeps the typed name, because a list that
// could not be fetched is not a list of one.
async function chooseBackend(gateway, model) {
  if (!gateway) return;
  const sent = gateway;
  const served = await loadModelsFor(sent);
  let chosen = (model || "").trim();
  if (served && served.length > 0) {
    if (!served.includes(chosen)) chosen = served[0];
  } else {
    chosen = chosen || PU.backendModelShown || "";
  }
  if (!chosen) {
    setBackendError("Pick a model for this gateway.");
    return;
  }
  try {
    const data = await fetchJson("/api/backend", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ gateway: sent, model: chosen }),
    });
    if (!data.success) {
      setBackendError(data.error || "The backend could not be changed");
      await loadBackends();
      return;
    }
    setBackendError("");
    await loadBackends();
  } catch (e) {
    setBackendError(`The backend could not be changed: ${e.message}`);
    await loadBackends();
  }
}

// Editing a turn of yours puts its words in the composer and leaves the bubble alone.
//
// The bubble used to become its own form: a field where the sentence was, a Save and a Cancel
// under it, and a third bar afterwards offering to send what had just been saved. Three controls
// for one edit, and two of them said the same thing as the composer already said. The composer is
// the one place on the page that asks the server for a turn, so an edit is that: the words go
// there, and the button under them does the only thing an edit can end with.
//
// What the edit changes is which message the next send replaces. `PU.editing` holds the bubble
// and the text it held when the field was filled, so a send that leaves the text alone costs
// nothing and a send that changes it steps the store back to just before the edited turn. Every
// later message was an answer to the old words, so it goes; that is what the note in the
// composer says while the edit is in flight.
//
// `fromButton` is set by the menu item, which is not a click on the bubble: the guards below
// exist to keep a drag that left a selection, or a press that landed on a link or a fold, from
// opening the composer. The menu has already made those decisions before it drew itself, so its
// word is taken rather than re-litigated.
function beginEdit(el, event, fromButton) {
  // Selecting text to copy it also clicks. A drag that leaves a selection is not a request to
  // edit, and neither is a click that landed on a fold or a link inside the bubble.
  if (!fromButton && event &&
      event.target.closest("a, button, .block-tool, .block-subtask, .block-thinking")) {
    return;
  }
  const selection = window.getSelection();
  if (selection && selection.toString().trim()) return;
  if (PU.isStreaming) {
    createSystemMessage("Cannot edit while a reply is streaming.");
    return;
  }
  const turn = turnOf(el);
  if (!turn) {
    createSystemMessage("This message has no turn to edit.");
    return;
  }
  // One edit at a time. Starting a second would overwrite the first without a way back to it.
  if (PU.editing && PU.editing.el === el) {
    PU.composer.inputEl.focus();
    return;
  }
  if (PU.editing) cancelEdit(false);
  PU.editing = { el, turn, original: el.dataset.text || "" };
  el.classList.add("editing");
  PU.composer.inputEl.value = PU.editing.original;
  resizeComposerInput();
  setComposerMode("edit");
  PU.composer.inputEl.focus();
  // The caret goes to the end: an edit usually appends or trims, and a field focused with the
  // caret at zero makes the reader press End before changing the half of the sentence they meant.
  PU.composer.inputEl.setSelectionRange(PU.composer.inputEl.value.length,
                                         PU.composer.inputEl.value.length);
}

// Puts the composer back to what it does between turns. `restore` is false when the caller is
// about to overwrite the field anyway (a new edit, or a send that has taken the text), and true
// when the words should go back to what the edited turn still says: leaving a cancelled sentence
// in the box would invite a send that says it as a new turn instead of the edit that was dropped.
function cancelEdit(restore) {
  const editing = PU.editing;
  PU.editing = null;
  if (editing) editing.el.classList.remove("editing");
  setComposerMode("new");
  if (restore && !PU.isStreaming) {
    PU.composer.inputEl.value = "";
    resizeComposerInput();
  }
}

// The composer says which of its two jobs it is doing. The label is on the button rather than in
// a banner above the field: the button is what changes meaning, so the button is what should say
// so, and there is then nothing extra on the page to dismiss.
function setComposerMode(mode) {
  PU.composer.mode = mode;
  if (mode === "edit") {
    PU.composer.inputEl.placeholder =
        "Editing this message… (Enter to replace it, Shift+Enter for a newline, Escape to cancel)";
    PU.composer.inputEl.dataset.editing = "1";
    renderSendButton();
    return;
  }
  delete PU.composer.inputEl.dataset.editing;
  PU.composer.inputEl.placeholder =
      "Type a message… (Enter to send, Shift+Enter for a newline)";
  renderSendButton();
}

// A send that lands while an edit is open is the edit. The turn is the one the bubble carries, so
// the store steps back to just before it and the text replaces it; nothing is appended.
//
// A step back that the server refuses leaves the conversation as it was and the words in the
// field, because the alternative is a message already removed from the page and still held by the
// store. That is the one failure the reader has to be told about, so it is the one this returns.
async function sendEdit() {
  const editing = PU.editing;
  if (!editing) return false;
  const text = PU.composer.inputEl.value.trim();
  if (!text) return false;
  // The same words are not an edit. Dropping the mode is the whole of what is left to do, and
  // asking the server to step back for a message it would then be told to say again is a round
  // trip that changes nothing.
  if (text === editing.original) {
    cancelEdit(true);
    return false;
  }
  // Everything from the edited turn on is what the store is about to lose: the old words, and
  // every reply that was an answer to them. The step back happens before the text is offered for
  // sending, so the composer's next turn number and the store's chain agree by the time the send
  // is made.
  const dropped = [];
  for (let node = editing.el; node; node = node.nextElementSibling) {
    dropped.push(node);
  }
  try {
    await postJson("/api/rewind", { turn: editing.turn });
  } catch (e) {
    createSystemMessage("Could not step back: " + e.message);
    return false;
  }
  for (const node of dropped) node.remove();
  // A reply still being written belonged to a turn just dropped: it must not keep drawing into a
  // conversation that has been stepped back under it.
  if (PU.currentAssistantEl && !PU.composer.messagesEl.contains(PU.currentAssistantEl)) {
    PU.currentAssistantEl = null;
    PU.currentAssistantBlocks = [];
  }
  PU.editing = null;
  editing.el.classList.remove("editing");
  setComposerMode("new");
  PU.composer.inputEl.value = "";
  resizeComposerInput();
  await refreshChainLength();
  // The turn is drawn again from nothing rather than kept as the bubble that was edited: it is
  // no longer the message the store holds, and the reply that follows is drawn into a transcript
  // the server has stepped back to. `existing` is left out so the send creates the bubble.
  return sendText(text, null, editing.turn);
}


