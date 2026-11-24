// SPDX-License-Identifier: GPL-3.0-only
// Where the conversation goes: the gateway and its address, and -- from the bar, where the model is
// already written -- which model answers.

"use strict";

// ---------- the gateway form ----------

// The form opens on what is in force rather than on what was last typed. Every open starts from
// the server's answer, so a draft abandoned last time cannot return as if it had been saved.
function gatewayFill() {
  const { gatewaySelect, hostInput } = PU.composer;
  if (gatewaySelect) gatewaySelect.value = PU.gatewayShown || "";
  if (hostInput) hostInput.value = PU.backendHostShown || "";
  setBackendError("");
}

function gatewayDialog() {
  return document.getElementById("gateway-dialog");
}

// Opening asks the gateway in force what it serves, so the model menu in the bar is ready the
// moment the reader presses it. The answer lands asynchronously, which is why the fill happens
// first: the form is usable at once, and the list arrives under it.
function gatewayOpen() {
  const dialog = gatewayDialog();
  if (!dialog || dialog.open) return;
  if (PU.isStreaming) {
    createSystemMessage("A reply is still arriving. Wait for it to finish, or stop it, before changing where it goes.");
    return;
  }
  gatewayFill();
  loadModelsFor(PU.gatewayShown || (PU.composer.gatewaySelect ? PU.composer.gatewaySelect.value : ""));
  dialog.showModal();
}

function gatewayClose() {
  const dialog = gatewayDialog();
  if (dialog && dialog.open) dialog.close();
}

// Save is the one path that writes. It closes only when the server took the answers: a refusal is
// the form's to show, and a form that closed on a refusal would leave the reader believing the
// change was made.
//
// The button is held while the request runs, because the request can take as long as a connect
// timeout -- a gateway whose host does not answer is asked what it serves before the backend is
// written -- and a button that looked untouched through those seconds reads as one that did not
// take the press. `aria-busy` says the same thing to a reader who is not watching the label.
async function withBusy(button, work) {
  if (!button) return work();
  if (button.getAttribute("aria-busy") === "true") return undefined;
  const label = button.textContent;
  button.setAttribute("aria-busy", "true");
  button.disabled = true;
  button.textContent = "Saving…";
  try {
    return await work();
  } finally {
    button.removeAttribute("aria-busy");
    button.disabled = false;
    button.textContent = label;
  }
}

async function gatewaySave() {
  const ok = await withBusy(document.getElementById("gw-save"), saveGateway);
  if (ok) gatewayClose();
}

function wireGatewayDialog() {
  const dialog = gatewayDialog();
  if (!dialog) return;
  // A button presses itself: Enter and Space arrive as a `click`, so there is no key handler to
  // write here and no way for the two targets to drift apart in how they answer the keyboard.
  const open = document.getElementById("status-gateway");
  if (open) open.addEventListener("click", gatewayOpen);
  const save = document.getElementById("gw-save");
  if (save) save.addEventListener("click", gatewaySave);
  const cancel = document.getElementById("gw-cancel");
  if (cancel) cancel.addEventListener("click", gatewayClose);
  // Cancel is the only button that closes this sheet: the head has no ✕, since a corner one would
  // be the same exit as the pair at the foot, both on screen at once. The rest of the ways out --
  // Escape and a backdrop click, wired below -- are gestures rather than controls and so do not
  // read as a second button.
  // The buttons are type=button, so every way out goes through gatewayClose: Enter in the address
  // field must not submit the head's form and close the sheet before the server has answered.
  const form = document.getElementById("gateway-form");
  if (form) form.addEventListener("submit", (e) => e.preventDefault());
  dialog.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && e.target.tagName !== "SELECT") {
      e.preventDefault();
      gatewayClose();
    }
  });
  // A click on the backdrop lands on the dialog element itself, since the scroll box inside it
  // fills the box.
  dialog.addEventListener("click", (e) => {
    if (e.target === dialog) gatewayClose();
  });
}

// ---------- the model form ----------
//
// The model is a form and not a list because it is more than a name: the same gateway will answer
// at any temperature, under any cap, against any window, and those are the settings a reader
// changes when the reply is too bold or the prompt is too long -- not the gateway, which is where
// the turn is sent rather than how it is answered. Splitting them this way keeps the two presses
// in the bar answering the two questions the bar shows.

function modelDialog() {
  return document.getElementById("model-dialog");
}

function modelField(id) {
  return document.getElementById(id);
}

// A field a form may or may not be drawing: empty rather than a missing element, so a reader
// writes one call where they would otherwise look the same id up twice to ask whether it is
// there.
function fieldValue(id) {
  const el = modelField(id);
  return el ? el.value : "";
}

// A float arriving over the wire is the nearest double to a float32, so 0.7 comes back as
// 0.699999988079071: the server's number is right and its decimal is an artefact of the conversion,
// and printing it would show the reader a precision nobody chose. Six places is more than any
// sampling setting needs and short enough that the artefact is gone; the number itself is not
// changed, only how it is written in a box the reader may never touch.
function showNumber(value) {
  if (value === null || value === undefined) return "";
  const n = Number(value);
  if (!Number.isFinite(n)) return "";
  return String(Math.round(n * 1e6) / 1e6);
}

// The catalogue is the menu's option list, rebuilt in place. The name in force is always one of the
// entries: a catalogue is what a gateway says it serves, which is not always everything it answers,
// and a name that is running must be selectable back out of the menu it sits in. It goes first when
// the catalogue does not list it, so the menu opens on the name in force either way -- and a
// catalogue that could not be read leaves a menu of one, which is the honest drawing of a gateway
// that could not be asked rather than an empty field the reader has to guess into.
//
// The one case a menu cannot answer is no entries at all, and that is a state a gateway really
// states: tencent answers `"agents":null,"models":null` for an account with no models configured.
// A menu of nothing is a dead end -- there is nothing to pick and no way to type out of it -- so
// the box takes over in that case and the menu hides. They are never both shown, so the form still
// holds one answer in one place.
function fillModelChoices(names, current, error) {
  const menu = modelField("md-name");
  const typed = modelField("md-typed");
  const menuField = modelField("md-name-field");
  const typedField = modelField("md-typed-field");
  if (!menu) return;
  const known = (names || []).slice();
  const shown = current === undefined ? PU.backendModelShown || "" : current;
  // Whether the *gateway* offered anything, asked before the name in force is folded in: that
  // name is added so it stays selectable, and counting it here would let a gateway that lists
  // nothing look like a gateway with one entry -- the reason for the emptiness would be lost.
  const empty = known.length === 0;
  if (shown && !known.includes(shown)) known.unshift(shown);

  // Which control holds the answer, set here rather than re-derived at Save from what looks
  // hidden: one place decides, so the two cannot disagree.
  PU.modelTyped = empty;
  menu.textContent = "";
  if (menuField) menuField.hidden = empty;
  if (typedField) typedField.hidden = !empty;
  // The name is not lost by switching control: whatever was the answer moves across, with the
  // name in force as the fallback, so an empty catalogue does not also mean an empty box.
  if (empty) {
    if (typed) typed.value = shown;
    // Two ways to arrive at an empty list, and they are not the same fact. A catalogue that could
    // not be read says nothing about what the gateway serves, so the reader is told why and left
    // to name what they know works. A catalogue that answered and listed nothing is the gateway
    // saying its account has no models, which is worth saying in its own words rather than
    // inviting a name that will be refused.
    if (error) {
      modelError("The catalogue could not be read (" + error + "). Name a model this gateway serves.");
    } else if (shown) {
      modelError("This account's catalogue is empty; the name in force is offered until you name another.");
    }
    return;
  }
  for (const name of known) menu.append(new Option(name, name));
  // Assigned after the options exist: a value set before its option was drawn is dropped by the
  // browser, and the menu would open on its first entry instead of on the name in force.
  if (shown) menu.value = shown;
  // A menu with entries has nothing to explain, and whatever the last fill said was about a
  // catalogue that is no longer the one on screen.
  modelError("");
}

// The form opens on what is in force rather than on what was last typed, for the same reason the
// gateway's form does: a draft abandoned last time must not return as if it had been saved. An
// unstated setting -- no cap, no seed -- shows as an empty box rather than as a number, because a
// cap nobody chose is not a cap.
function modelFill() {
  const current = PU.backendModelShown || "";
  fillModelChoices(PU.modelsShown || [], current);
  const numbers = {
    "md-temperature": PU.backendParamsShown.temperature,
    "md-max-tokens": PU.backendParamsShown.max_tokens,
    "md-top-p": PU.backendParamsShown.top_p,
    "md-seed": PU.backendParamsShown.seed,
    "md-context-window": PU.backendParamsShown.context_window,
  };
  for (const [id, value] of Object.entries(numbers)) {
    const input = modelField(id);
    if (input) input.value = showNumber(value);
  }
  // The level is a menu rather than a box, so it is filled from the value in force and falls back
  // to `auto` when nothing is stated: a level the menu does not offer would leave it silently on
  // its first option, and falling back on purpose is not the same thing.
  const thinking = modelField("md-thinking");
  if (thinking) {
    const level = PU.backendParamsShown.thinking;
    const named = level === undefined || level === null || level === "" ? "auto" : String(level);
    const known = Array.from(thinking.options, (option) => option.value);
    thinking.value = known.includes(named) ? named : "auto";
  }
  modelError("");
}

function modelError(message) {
  const el = modelField("md-error");
  if (!el) return;
  el.textContent = message || "";
  el.hidden = !message;
}

// Opening asks the gateway in force what it serves, so the name field has a menu when the
// catalogue is readable and a box when it is not. The fill happens first and the list arrives
// under it: the form is usable the moment it opens, and a gateway that never answers costs no wait.
//
// The arriving catalogue changes which control holds the name, and nothing else. It must not
// re-fill the whole form: the reader may already have typed a temperature by the time the list
// lands, and a re-fill would quietly undo it -- the same defect a Save-then-redraw had, where a
// late fill put the old numbers back under a reader who had just changed them. It must not move
// the choice either: a name picked or typed while the catalogue was in flight is the reader's
// answer and not a value to be corrected.
function modelOpen() {
  const dialog = modelDialog();
  if (!dialog || dialog.open) return;
  if (PU.isStreaming) {
    createSystemMessage("A reply is still arriving. Wait for it to finish, or stop it, before changing the model.");
    return;
  }
  modelFill();
  dialog.showModal();
  const gateway = PU.gatewayShown || "";
  const picked = PU.modelTyped
    ? fieldValue("md-typed")
    : fieldValue("md-name");
  loadModelsFor(gateway).then((entry) => {
    if (!dialog.open) return;
    // No `modelError("")` here: whether there is anything to say about an empty list is the
    // fill's to decide, and clearing after it would wipe the reason it just wrote.
    fillModelChoices((entry && entry.models) || [], picked || PU.backendModelShown || "",
                     entry && entry.error);
  });
}

function modelClose() {
  const dialog = modelDialog();
  if (dialog && dialog.open) dialog.close();
}

// Save is the one path that writes, and it closes only when the server took the answers: a refusal
// is the form's to show, and a form that closed on one would leave the reader believing a change
// that was never made.
async function modelSave() {
  const menu = modelField("md-name");
  const typed = modelField("md-typed");
  // One answer, read from whichever control is showing: the menu when the catalogue had entries,
  // the box when it had none. `PU.modelTyped` is set by the same call that shows one and hides the
  // other, so the read cannot disagree with what the reader is looking at.
  const model = PU.modelTyped
    ? (typed ? typed.value.trim() : "")
    : (menu ? menu.value.trim() : "");
  if (!model) {
    modelError(PU.modelTyped
      ? "Name a model: this account's catalogue is empty, so nothing is listed to pick."
      : "Pick a model from the list.");
    return;
  }
  const raw = {
    temperature: fieldValue("md-temperature").trim(),
    max_tokens: fieldValue("md-max-tokens").trim(),
    top_p: fieldValue("md-top-p").trim(),
    seed: fieldValue("md-seed").trim(),
    context_window: fieldValue("md-context-window").trim(),
    // A menu always holds a value, so there is no empty case to send as null: `auto` is the
    // reader's word for "no level of our own" and travels as itself.
    thinking: fieldValue("md-thinking") || "auto",
  };
  // The same wait as the gateway form's, for the same reason, though this form's request does not
  // go through a connect timeout of its own: it is the same server writing the same file.
  const ok = await withBusy(modelField("md-save"), () => saveModel(model, raw));
  if (ok) modelClose();
}

function wireModelDialog() {
  const dialog = modelDialog();
  if (!dialog) return;
  const open = document.getElementById("status-model");
  if (open) open.addEventListener("click", modelOpen);
  // The name box is the answer, so there is no second control to keep in step with it: the
  // suggestions are what the browser draws from the datalist and nothing here mirrors them back.
  const save = modelField("md-save");
  if (save) save.addEventListener("click", modelSave);
  const cancel = modelField("md-cancel");
  if (cancel) cancel.addEventListener("click", modelClose);
  // Same as the gateway sheet's head: Cancel is the one control that leaves, because the ✕ that
  // used to sit in the corner said what Cancel already says. Escape and a backdrop click remain,
  // and neither is drawn as a button.
  // The buttons are type=button, so every way out goes through modelClose: Enter in a number box
  // must not submit the head's form and close the sheet before the server has answered.
  const form = modelField("model-form");
  if (form) form.addEventListener("submit", (e) => e.preventDefault());
  dialog.addEventListener("keydown", (e) => {
    // A menu open on Escape is the browser's to close and the form's to survive: the level's list
    // is the sheet's last native menu, and closing the whole form instead would throw away every
    // answer given so far for the sake of dismissing one of them.
    if (e.key === "Escape" && e.target.tagName !== "SELECT") {
      e.preventDefault();
      modelClose();
    }
  });
  // A click on the backdrop lands on the dialog element itself, since the scroll box inside it
  // fills the box.
  dialog.addEventListener("click", (e) => {
    if (e.target === dialog) modelClose();
  });
}

wireGatewayDialog();
wireModelDialog();


