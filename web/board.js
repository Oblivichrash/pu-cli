// SPDX-License-Identifier: GPL-3.0-only
// The board the page reads: who holds what, what is late, and who is being asked something.
// A read-out like the rest of the panel — the only write here is answering a blocked job,
// because a question with no way to answer it is the one thing the page must not leave
// hanging.
//
// Nothing is drawn when the board is empty: a session with no peers in play costs no row and
// no request beyond the one that found it empty.
//
// The read is `/api/board/local`, not `/api/jobs`: a peer's read presents the shared token and
// counts as being heard from, and a page has no token to present.

"use strict";

const BOARD_POLL_MS = 5000;
// A closed job is worth reading until it is stale; the server keeps its own longer history.
const BOARD_CLOSED_SHOWN = 3;

function boardStateLabel(job) {
  if (Number(job.overdue)) return job.state + " · late";
  return job.state;
}

function boardQuestion(job, onAnswer) {
  const row = document.createElement("div");
  row.className = "board-question";

  const asked = document.createElement("div");
  asked.className = "board-asked";
  asked.textContent = job.question;
  row.appendChild(asked);

  const answer = document.createElement("input");
  answer.type = "text";
  answer.placeholder = "Answer the question…";
  row.appendChild(answer);

  const send = document.createElement("button");
  send.type = "button";
  send.textContent = "Answer";
  // An empty answer is not an answer: the route refuses it, so the button waits for one.
  send.disabled = true;
  answer.addEventListener("input", () => { send.disabled = answer.value.trim() === ""; });
  answer.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !send.disabled) send.click();
  });
  send.addEventListener("click", () => onAnswer(job.id, answer.value.trim()));
  row.appendChild(send);

  return row;
}

function boardJobRow(job, onAnswer) {
  const item = document.createElement("li");
  item.className = "board-job";

  const head = document.createElement("div");
  head.className = "board-job-head";

  const state = document.createElement("span");
  state.className = "board-state board-state-" + job.state + (Number(job.overdue) ? " late" : "");
  state.textContent = boardStateLabel(job);
  head.appendChild(state);

  const intent = document.createElement("span");
  intent.className = "board-intent";
  intent.textContent = job.intent;
  intent.title = job.intent;
  head.appendChild(intent);

  item.appendChild(head);

  const meta = document.createElement("div");
  meta.className = "board-meta";
  meta.textContent = job.from ? `${job.from} → ${job.to}` : job.to;
  item.appendChild(meta);

  // The server decides what a question is; the page does not guess from the state name.
  if (job.state === "blocked" && job.question) item.appendChild(boardQuestion(job, onAnswer));

  if (job.note) {
    const note = document.createElement("div");
    note.className = "board-note";
    note.textContent = job.note;
    item.appendChild(note);
  }
  return item;
}

function renderBoard(snapshot) {
  const panel = document.getElementById("panel-board");
  const list = document.getElementById("board-jobs");
  if (!panel || !list) return;

  const jobs = Array.isArray(snapshot && snapshot.jobs) ? snapshot.jobs : [];
  const open = jobs.filter((job) => job.state !== "done" && job.state !== "failed");
  const closed = jobs.filter((job) => job.state === "done" || job.state === "failed").slice(-BOARD_CLOSED_SHOWN);

  // A board nobody is using is not a panel with nothing in it; it is no panel.
  panel.hidden = open.length === 0 && closed.length === 0;
  list.replaceChildren();
  if (panel.hidden) return;

  const onAnswer = (id, answer) => answerBoardJob(id, answer);
  for (const job of open) list.appendChild(boardJobRow(job, onAnswer));

  if (closed.length > 0) {
    const heading = document.createElement("li");
    heading.className = "board-heading";
    heading.textContent = "Settled";
    list.appendChild(heading);
    for (const job of closed) list.appendChild(boardJobRow(job, onAnswer));
  }
}

async function refreshBoard() {
  try {
    renderBoard(await fetchJson("/api/board/local"));
  } catch (_) {
    // A board that cannot be read is not worth drawing a wrong one over; the next poll tries.
  }
}

async function answerBoardJob(id, answer) {
  try {
    const res = await fetch("/api/jobs/answer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ job: id, answer }),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    await refreshBoard();
  } catch (_) {
    showNotice("That answer did not reach the board.");
  }
}

function startBoardPolling() {
  refreshBoard();
  setInterval(refreshBoard, BOARD_POLL_MS);
}
