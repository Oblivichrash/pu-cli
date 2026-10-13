// SPDX-License-Identifier: GPL-3.0-only
// The approval gate's page half: a bar that parks under the composer while the
// turn thread waits for an answer, taken down by either button.

let approvalPending = null;

function showApprovalBar(command) {
  approvalPending = command;
  let bar = document.getElementById("approval-bar");
  if (!bar) {
    bar = document.createElement("div");
    bar.id = "approval-bar";
    bar.innerHTML =
      '<span id="approval-command"></span>' +
      '<input id="approval-note" placeholder="Optional note…">' +
      '<button id="approval-allow">Approve</button>' +
      '<button id="approval-deny">Deny</button>';
    document.body.appendChild(bar);
    document.getElementById("approval-allow").addEventListener("click",
      () => resolveApproval(true));
    document.getElementById("approval-deny").addEventListener("click",
      () => resolveApproval(false));
  }
  document.getElementById("approval-command").textContent = command;
  document.getElementById("approval-note").value = "";
  bar.hidden = false;
}

function resolveApproval(allow) {
  if (approvalPending === null) return;
  const note = document.getElementById("approval-note").value.trim();
  ws.send(JSON.stringify(
    { type: "resolve_approval", payload: { allow, text: note } }));
  approvalPending = null;
  document.getElementById("approval-bar").hidden = true;
}
