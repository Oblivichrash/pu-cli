#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only

//
// The header's server strip and the panel's spend figure, checked as the page runs them.
//
// Both bugs this guards were structural rather than typographic. The strip drew a placeholder
// until the registry answered, so a server that had not registered itself yet left the page
// claiming not to know where it was. The spend figure was drawn once at load, so a turn that
// finished during the visit never appeared in it: the number was right, and it described a
// conversation that had ended before the one on screen.
//
// The strip's third bug is the one that makes the first two recur: the page learned its own name
// from the list of *other* servers, so a page that could not read that list — every page opened
// over the network, since the route answers loopback only — had no name at all. The cases below
// drive the fetcher rather than the renderer, because a case that called `renderServers()`
// directly could not have seen it.
//
// The stubs are the smallest things the modules touch, not a browser: an element that
// remembers its text and whether it is hidden, and an id lookup over a fixed set.
//

const {readFileSync} = require("node:fs");
const {join} = require("node:path");
const vm = require("node:vm");

const web = join(__dirname, "..", "..", "web");

function makeElement(id) {
  return {
    id,
    textContent: "",
    title: "",
    hidden: false,
    disabled: false,
    className: "",
    children: [],
    // showUsage writes the context bar's width, so an element has to carry a style the way a
    // real one does; a stub without it fails for a reason the page never would.
    style: {},
    replaceChildren() {
      this.children = [];
    },
    appendChild(child) {
      this.children.push(child);
    },
    addEventListener() {},
  };
}

function makeDocument(ids) {
  const elements = new Map(ids.map((id) => [id, makeElement(id)]));
  return {
    elements,
    getElementById: (id) => elements.get(id) || null,
    createElement: (tag) => makeElement(tag),
  };
}

const IDS = [
  "server-toggle", "server-list", "server-current",
  "context-usage", "context-fill", "usage-total", "status-context",
];

// A page opened on `origin`. The default is a loopback page, which is the one that can read the
// server list; `remote` is the one that cannot, and is the page this whole file is about.
function makeWindow(origin) {
  const url = new URL(origin);
  return {
    location: {
      origin: url.origin,
      protocol: url.protocol,
      hostname: url.hostname,
      port: url.port,
      host: url.host,
    },
  };
}

function loadPage(fetchImpl, origin = "http://127.0.0.1:8792") {
  const document = makeDocument(IDS);
  const context = vm.createContext({
    console,
    document,
    URL,
    // The browser's one way out, so the stub is the thing `api.js` calls rather than a function
    // this file defines over the top of it: a case that replaced `fetchJson` would test its own
    // stub, and the status-to-meaning rule is what these cases are about.
    fetch: fetchImpl || (async () => jsonResponse({})),
    window: makeWindow(origin),
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
  });
  // api.js is in the list because the transport is what these cases drive: a case that stubbed a
  // renderer could not have seen a failure that happens before the renderer is reached.
  for (const file of ["blocks.js", "state.js", "api.js", "panel.js", "servers.js"]) {
    vm.runInContext(readFileSync(join(web, file), "utf8"), context, {filename: file});
  }
  // refreshChainLength is the turn-end path, so the file that holds it is loaded the same way.
  vm.runInContext(readFileSync(join(web, "session.js"), "utf8"), context, {filename: "session.js"});
  // state.js declares PU as a top-level const, which the module's own scope holds rather than
  // the context's global object. Reading it here is what the page does; writing it is what a
  // case has to do, so the handle is taken from inside the scope the files ran in.
  vm.runInContext(
      "globalThis.page = {get PU() { return PU; }, renderServers, renderUsageTotal, showUsage, " +
      "refreshChainLength, setSelf, setOthers, siblingOrigin, refreshServers};",
      context);
  return {context, document, page: context.page};
}

// A response as `fetch` hands one over: the status, whether it was a success, and a body that
// parses once. `json` is a method because the page calls it as one.
function jsonResponse(body, status = 200) {
  return {
    ok: status >= 200 && status < 300,
    status,
    async json() {
      return body;
    },
  };
}

// A fetcher that answers each path with one canned result, and records what was asked for. An
// answer is either a response object or a thrown error, so a network failure and a refusal are
// told apart the way the browser tells them apart: one rejects, the other resolves with a status.
function stubFetch(answers) {
  const asked = [];
  const fetchImpl = async (url) => {
    const path = new URL(url).pathname;
    asked.push(path);
    const answer = answers[path];
    if (answer === undefined) throw new Error(`no stub for ${path}`);
    if (answer instanceof Error) throw answer;
    // A response is already a response; a plain object is the body of a `200`.
    if (typeof answer.json === "function") return answer;
    return jsonResponse(answer);
  };
  return {asked, fetchImpl};
}

// The refusal the route gives a page that is not on the server's machine.
function refusal(status = 403) {
  return jsonResponse({success: false, error: "read on the machine"}, status);
}

let failed = 0;
const pending = [];

// Async cases exist because the turn-end path is async: a check that awaited nothing would
// report success for a page that never asked. A rejection in one of those has to reach the
// counter, or the run prints "all cases passed" over a failure that did happen.
function check(name, body) {
  const settle = (error) => {
    if (!error) {
      console.log(`ok   ${name}`);
      return;
    }
    failed += 1;
    console.error(`FAIL ${name}`);
    console.error(`  ${error.message}`);
  };
  if (body.constructor.name === "AsyncFunction") {
    pending.push(body().then(() => settle(null), settle));
    return;
  }
  try {
    body();
    settle(null);
  } catch (error) {
    settle(error);
  }
}

function equal(actual, expected, what) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

// --- The name in the header ------------------------------------------------------------------

check("The strip names the workspace the server reported", () => {
  const {page, document} = loadPage();
  page.setSelf({name: "a/pu-cli", port: 8792});
  page.setOthers({servers: []});
  page.renderServers();
  equal(document.elements.get("server-current").textContent, "a/pu-cli", "name");
  if (!document.elements.get("server-current").title.includes("127.0.0.1:8792")) {
    throw new Error(`tooltip: ${document.elements.get("server-current").title}`);
  }
});

// The page was served by a server, so it always knows where it is. Before `/api/session` answers,
// the label is the address that served this document; it is never the placeholder, and it does not
// depend on the list of other servers, which a remote page can never read.
check("A page whose server has not named itself yet shows its own address", () => {
  const {page, document} = loadPage();
  page.setSelf({name: "", port: null});
  page.setOthers({servers: []});
  page.renderServers();
  const name = document.elements.get("server-current");
  equal(name.textContent, "http://127.0.0.1:8792", "name");
  if (name.textContent === "—") throw new Error("the page kept its placeholder");
});

// The bug this file was extended for: a remote page gets `403` from `/api/servers`, and the header
// has to draw anyway. It named itself from the list before, so the refusal left it blank.
check("A remote page still names itself when the server list is refused", async () => {
  const {fetchImpl} = stubFetch({"/api/servers": refusal(403)});
  const {page, document} = loadPage(fetchImpl, "http://192.168.1.50:9000");
  page.setSelf({name: "", port: null});

  await page.refreshServers();

  const name = document.elements.get("server-current");
  equal(name.textContent, "http://192.168.1.50:9000", "name");
  if (name.textContent === "—") throw new Error("the page kept its placeholder");
  equal(page.PU.others.length, 0, "no other servers listed");
  equal(page.PU.othersRestricted, "loopback", "the reason it has none");
});

// A refusal and a network failure are different answers, and the header says which: a machine with
// one server is not the same as a route this page could not reach.
check("The caret says why it cannot be used", async () => {
  const {fetchImpl} = stubFetch({"/api/servers": refusal(403)});
  const {page, document} = loadPage(fetchImpl, "http://192.168.1.50:9000");
  await page.refreshServers();
  page.renderServers();
  const caret = document.elements.get("server-toggle");
  equal(caret.disabled, true, "caret disabled");
  if (!caret.title.toLowerCase().includes("machine")) {
    throw new Error(`caret title: ${caret.title}`);
  }

  const offline = loadPage(async () => { throw new Error("network down"); });
  await offline.page.refreshServers();
  offline.page.renderServers();
  equal(offline.page.PU.othersRestricted, "unreachable", "the other reason");
  if (!offline.document.elements.get("server-toggle").title.toLowerCase().includes("could not")) {
    throw new Error(`caret title: ${offline.document.elements.get("server-toggle").title}`);
  }
});

check("The caret stays where a second server would appear", async () => {
  const {fetchImpl} = stubFetch({"/api/servers": {servers: [], success: true}});
  const {page, document} = loadPage(fetchImpl);
  await page.refreshServers();
  page.renderServers();
  equal(document.elements.get("server-toggle").hidden, false, "caret hidden");
  equal(document.elements.get("server-list").hidden, true, "list hidden");
  equal(document.elements.get("server-current").disabled, true, "name disabled");
});

check("A second server fills the list and the caret opens it", async () => {
  const {fetchImpl} = stubFetch({
    "/api/servers": {servers: [{name: "a/other", port: 8793}], success: true},
  });
  const {page, document} = loadPage(fetchImpl);
  await page.refreshServers();
  page.renderServers();
  const list = document.elements.get("server-list");
  equal(list.children.length, 1, "rows");
  equal(list.children[0].children[0].textContent, "a/other", "row label");
  equal(document.elements.get("server-current").disabled, false, "name disabled");
  equal(document.elements.get("server-toggle").disabled, false, "caret disabled");
});

// A sibling is the port on the host this page already reached. The bind address is not a
// destination: `0.0.0.0` is a legal place to listen and never an address a browser can dial, and a
// page that built its origin from one would send every request to its own loopback.
check("A sibling's address keeps the host this page reached", () => {
  const {page} = loadPage(undefined, "http://192.168.1.50:9000");
  equal(page.siblingOrigin(9001), "http://192.168.1.50:9001", "same host, other port");
  if (page.siblingOrigin(9001).includes("0.0.0.0")) {
    throw new Error("the bind address was used as a destination");
  }
});

// Naming a server and asking it for a transcript are one act: the header reads `PU.self` and every
// REST call reads `PU.self.origin`. A page that wrote one and not the other would draw one
// workspace's name over another's conversation, and the bugs this file pins — a header that showed
// the placeholder, a caret that vanished — were both that split showing through. The writer is
// therefore the thing to test, not each caller of it.
check("Naming a server also moves the address the page talks to", () => {
  const {page} = loadPage();
  page.setSelf({origin: "http://127.0.0.1:8793", name: "a/other", port: 8793});
  equal(page.PU.self.origin, "http://127.0.0.1:8793", "origin");
  equal(page.PU.self.name, "a/other", "name");
  equal(page.PU.self.port, 8793, "port");

  page.setSelf({origin: "http://127.0.0.1:8792", name: "a/pu-cli", port: 8792});
  equal(page.PU.self.origin, "http://127.0.0.1:8792", "origin follows back");
  equal(page.PU.self.name, "a/pu-cli", "name follows back");

  // A caller that leaves a field out is not changing it: `setOthers` never touches the identity,
  // which is what keeps the list from being able to blank the name.
  page.setOthers({servers: [{name: "a/other", port: 8793}]});
  equal(page.PU.self.name, "a/pu-cli", "the identity survived a list read");
});

// --- The spend figure ------------------------------------------------------------------------

check("The spend figure redraws from the total the server sends", () => {
  const {page, document} = loadPage();
  page.renderUsageTotal({
    turns: 2, rounds: 3, prompt_tokens: 1000, completion_tokens: 50,
    cached_tokens: 0, priced_turns: 0,
  });
  const host = document.elements.get("usage-total");
  equal(host.children.length, 2, "lines");
  if (!host.children[0].textContent.includes("2 turns")) {
    throw new Error(`first line: ${host.children[0].textContent}`);
  }
});

// The page drew the figure once at load and never again, which is why a turn finished during
// the visit was missing from a total that was otherwise correct. The turn that ended is part
// of the total, so the frame that reports it carries the total too. The assertion is on the
// panel: a page that receives the total and does not draw it is the same bug.
check("A finished turn carries the total it is part of", async () => {
  const {page, document} = loadPage();
  page.renderUsageTotal({turns: 6, rounds: 6, prompt_tokens: 10, completion_tokens: 2, cached_tokens: 0, priced_turns: 0});
  const before = document.elements.get("usage-total").children[0].textContent;
  page.showUsage({
    prompt_tokens: 10, completion_tokens: 2, cached_tokens: 4, rounds: 1, model: "m", gateway: "g",
    total: {turns: 7, rounds: 7, prompt_tokens: 20, completion_tokens: 4, cached_tokens: 4, priced_turns: 0},
  });
  const after = document.elements.get("usage-total").children[0].textContent;
  if (!before.includes("6 turns")) throw new Error(`before the turn ended: ${before}`);
  if (!after.includes("7 turns")) throw new Error(`after the turn ended: ${after}`);
  equal(document.elements.get("context-usage").textContent.length > 0, true, "this turn drawn");
});

// The frame and `/api/history` write a turn the same way, so a reload and a live turn draw the
// same blocks. The frame's own counts are the ones the bar reads, with the cache figure the
// panel needs already there rather than asked for separately.
check("One frame describes a turn and its total the same way a reload does", async () => {
  const {page, document} = loadPage();
  page.showUsage({
    prompt_tokens: 1000, completion_tokens: 50, cached_tokens: 400, rounds: 2,
    model: "test-model", gateway: "ollama", cost: {amount: 0.25, unit: "credits"},
    total: {turns: 1, rounds: 2, prompt_tokens: 1000, completion_tokens: 50, cached_tokens: 400,
            priced_turns: 1, cost: {amount: 0.25, unit: "credits"}},
  });
  const bar = document.elements.get("context-usage").textContent;
  if (!bar.includes("1,000 tokens")) throw new Error(`bar: ${bar}`);
  if (!bar.includes("40% cached")) throw new Error(`the cache share is missing: ${bar}`);
  const total = document.elements.get("usage-total");
  const drawn = total.children.map((row) => row.textContent).join(" | ");
  if (!drawn.includes("1 turn")) throw new Error(`total: ${drawn}`);
  if (!drawn.includes("0.25 credits")) throw new Error(`price: ${drawn}`);
});

Promise.all(pending).then(() => {
  if (failed > 0) {
    console.error(`\n${failed} case(s) failed`);
    process.exit(1);
  }
  console.log("\nall cases passed");
});
