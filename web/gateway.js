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
async function gatewaySave() {
  const ok = await saveGateway();
  if (ok) gatewayClose();
}

function wireGatewayDialog() {
  const dialog = gatewayDialog();
  if (!dialog) return;
  const open = document.getElementById("status-gateway");
  if (open) {
    open.addEventListener("click", gatewayOpen);
    // These are spans given a role, so the browser does not press them for the reader.
    open.addEventListener("keydown", (e) => {
      if (e.key !== "Enter" && e.key !== " " && e.key !== "Spacebar") return;
      e.preventDefault();
      gatewayOpen();
    });
  }
  const save = document.getElementById("gw-save");
  if (save) save.addEventListener("click", gatewaySave);
  const cancel = document.getElementById("gw-cancel");
  if (cancel) cancel.addEventListener("click", gatewayClose);
  const close = dialog.querySelector(".config-close");
  if (close) close.addEventListener("click", gatewayClose);
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

// The form opens on what is in force rather than on what was last typed, for the same reason the
// gateway's form does: a draft abandoned last time must not return as if it had been saved. An
// unstated setting -- no cap, no seed -- shows as an empty box rather than as a number, because a
// cap nobody chose is not a cap.
function modelFill() {
  const nameSelect = modelField("md-name");
  const nameText = modelField("md-name-text");
  const current = PU.backendModelShown || "";
  if (nameSelect) {
    const known = (PU.modelsShown || []).slice();
    if (current && !known.includes(current)) known.unshift(current);
    nameSelect.textContent = "";
    for (const name of known) nameSelect.append(new Option(name, name));
    if (known.includes(current)) nameSelect.value = current;
  }
  if (nameText) nameText.value = current;
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
  modelError("");
}

function modelError(message) {
  const el = modelField("md-error");
  if (!el) return;
  el.textContent = message || "";
  el.hidden = !message;
}

// Opening asks the gateway in force what it serves, so the name field is a list when the catalogue
// is readable and a typed box when it is not. The fill happens first and the list arrives under it:
// the form is usable the moment it opens, and a gateway that never answers costs no wait.
//
// The arriving list only fills in the name choices. It must not re-fill the whole form: the reader
// may already have typed a temperature by the time the catalogue lands, and a re-fill would quietly
// undo it -- the same defect a Save-then-redraw had, where a late fill put the old numbers back
// under a reader who had just changed them.
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
  loadModelsFor(gateway).then((models) => {
    if (!dialog.open) return;
    const nameSelect = modelField("md-name");
    if (!nameSelect) return;
    const current = nameSelect.value || PU.backendModelShown || "";
    const known = (Array.isArray(models) ? models : []).slice();
    if (current && !known.includes(current)) known.unshift(current);
    nameSelect.textContent = "";
    for (const name of known) nameSelect.append(new Option(name, name));
    if (known.includes(current)) nameSelect.value = current;
    if (known.length > 0) modelError("");
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
  const nameSelect = modelField("md-name");
  const nameText = modelField("md-name-text");
  const typed = nameText ? nameText.value.trim() : "";
  const listed = nameSelect ? nameSelect.value : "";
  const model = typed || listed;
  if (!model) {
    modelError("Name a model, or pick one from the list.");
    return;
  }
  const raw = {
    temperature: modelField("md-temperature") ? modelField("md-temperature").value.trim() : "",
    max_tokens: modelField("md-max-tokens") ? modelField("md-max-tokens").value.trim() : "",
    top_p: modelField("md-top-p") ? modelField("md-top-p").value.trim() : "",
    seed: modelField("md-seed") ? modelField("md-seed").value.trim() : "",
    context_window: modelField("md-context-window")
        ? modelField("md-context-window").value.trim() : "",
  };
  const ok = await saveModel(model, raw);
  if (ok) modelClose();
}

function wireModelDialog() {
  const dialog = modelDialog();
  if (!dialog) return;
  const open = document.getElementById("status-model");
  if (open) {
    open.addEventListener("click", modelOpen);
    // These are spans given a role, so the browser does not press them for the reader.
    open.addEventListener("keydown", (e) => {
      if (e.key !== "Enter" && e.key !== " " && e.key !== "Spacebar") return;
      e.preventDefault();
      modelOpen();
    });
  }
  // A name picked from the list fills the typed box too, so one field holds the answer whichever
  // way it arrived. Typing over it is how a model the gateway did not list is named.
  const nameSelect = modelField("md-name");
  const nameText = modelField("md-name-text");
  if (nameSelect && nameText) {
    nameSelect.addEventListener("change", () => {
      if (nameSelect.value) nameText.value = nameSelect.value;
    });
  }
  const save = modelField("md-save");
  if (save) save.addEventListener("click", modelSave);
  const cancel = modelField("md-cancel");
  if (cancel) cancel.addEventListener("click", modelClose);
  const close = dialog.querySelector(".config-close");
  if (close) close.addEventListener("click", modelClose);
  // The buttons are type=button, so every way out goes through modelClose: Enter in a number box
  // must not submit the head's form and close the sheet before the server has answered.
  const form = modelField("model-form");
  if (form) form.addEventListener("submit", (e) => e.preventDefault());
  dialog.addEventListener("keydown", (e) => {
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


