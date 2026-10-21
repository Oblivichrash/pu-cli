// SPDX-License-Identifier: GPL-3.0-only
// The page's one way out to the server. Every REST call goes through `apiJson`, which keeps the
// status a failure arrived with: a caller that cannot tell "this route is answered on the machine
// only" from "the network is down" has no way to say so, and the page it draws for either is the
// same blank that reads as a bug.
//
// The address is `PU.self.origin`, which is the server this page is attached to. Nothing here
// decides which server that is; state.js does, and the socket reads the same field.

"use strict";

// A refusal, with the status and the body the server sent. The message is the server's own
// sentence when there is one, because it names the rule that was applied.
class ApiError extends Error {
  constructor(status, body) {
    super((body && body.error) || `HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.body = body;
  }
}

async function apiJson(path, init) {
  const res = await fetch(PU.self.origin + path, init);
  let body = null;
  try {
    body = await res.json();
  } catch (_) {
    // A refusal that is not JSON is still a refusal; `body` stays null and the status is the
    // whole of what the caller learns.
  }
  if (!res.ok) throw new ApiError(res.status, body);
  return body;
}

function fetchJson(path) {
  return apiJson(path);
}

function postJson(path, body) {
  return apiJson(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}
