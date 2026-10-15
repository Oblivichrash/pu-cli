// SPDX-License-Identifier: GPL-3.0-only
// The remark is a strip above the composer, not a line in the transcript: it is about the
// page's own state, so it must not read as part of the conversation.

"use strict";

function showNotice(text) {
  const bar = document.getElementById("notice-bar");
  if (!bar) return;
  bar.textContent = text;
  const close = document.createElement("button");
  close.type = "button";
  close.textContent = "×";
  close.setAttribute("aria-label", "Dismiss");
  close.onclick = hideNotice;
  bar.appendChild(close);
  bar.hidden = false;
}

function hideNotice() {
  const bar = document.getElementById("notice-bar");
  if (bar) bar.hidden = true;
}
