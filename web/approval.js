// SPDX-License-Identifier: GPL-3.0-only
// The approval gate's page half: a card parked above the composer while the turn
// thread waits for an answer. Approve is Enter, Deny is Esc, and the command is
// expandable -- a decision waited on is the one thing that must not be missed.

let approvalPending = null;

function showApprovalBar(command) {
  approvalPending = command;
  const bar = document.getElementById("approval-bar");
  const cmdEl = document.getElementById("approval-command");
  const noteEl = document.getElementById("approval-note");
  let expanded = false;

  cmdEl.textContent = command;
  cmdEl.classList.remove("expanded");
  cmdEl.onclick = () => {
    expanded = !expanded;
    cmdEl.classList.toggle("expanded", expanded);
  };
  noteEl.value = "";
  bar.hidden = false;
  noteEl.focus();
}

function hideApprovalBar() {
  document.getElementById("approval-bar").hidden = true;
}

function approvalVisible() {
  return approvalPending !== null;
}

function resolveApproval(allow) {
  if (approvalPending === null) return;
  const note = document.getElementById("approval-note").value.trim();
  ws.send(JSON.stringify(
    { type: "resolve_approval", payload: { allow, text: note } }));
  approvalPending = null;
  hideApprovalBar();
}

document.addEventListener("keydown", (e) => {
  if (!approvalVisible()) return;
  const inNote = e.target && e.target.id === "approval-note";
  if (e.key === "Escape") {
    e.preventDefault();
    resolveApproval(false);
  } else if (e.key === "Enter" && inNote && !e.shiftKey) {
    e.preventDefault();
    resolveApproval(true);
  }
});

document.getElementById("approval-allow").addEventListener("click",
  () => resolveApproval(true));
document.getElementById("approval-deny").addEventListener("click",
  () => resolveApproval(false));
