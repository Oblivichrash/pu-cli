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
// The stubs are the smallest things the two modules touch, not a browser: an element that
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

function loadPage(fetchJson) {
  const document = makeDocument(IDS);
  const context = vm.createContext({
    console,
    document,
    // A page global, so it has to exist before the modules that call it are run.
    fetchJson: fetchJson || (async () => null),
    window: {
      location: {hostname: "127.0.0.1", port: "8792", origin: "http://127.0.0.1:8792", protocol: "http:"},
    },
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
  });
  for (const file of ["state.js", "panel.js", "servers.js"]) {
    vm.runInContext(readFileSync(join(web, file), "utf8"), context, {filename: file});
  }
  // refreshChainLength is the turn-end path, so the file that holds it is loaded the same way.
  vm.runInContext(readFileSync(join(web, "session.js"), "utf8"), context, {filename: "session.js"});
  // state.js declares PU as a top-level const, which the module's own scope holds rather than
  // the context's global object. Reading it here is what the page does; writing it is what a
  // case has to do, so the handle is taken from inside the scope the files ran in.
  vm.runInContext(
      "globalThis.page = {get PU() { return PU; }, renderServers, renderUsageTotal, refreshChainLength};",
      context);
  return {context, document, page: context.page};
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

const ONE = {directory: "/home/a/pu-cli", host: "127.0.0.1", port: 8792, current: true};
const TWO = {directory: "/home/a/other", host: "127.0.0.1", port: 8793, current: false};

check("The strip names the workspace instead of the page's placeholder", () => {
  const {page, document} = loadPage();
  page.PU.server = ONE;
  page.PU.servers = [ONE];
  page.renderServers();
  equal(document.elements.get("server-current").textContent, "a/pu-cli", "name");
  equal(document.elements.get("server-current").title, ONE.directory, "tooltip");
});

// A server that has not written its registry entry is still the one answering, so the name
// comes from the address the page was opened on rather than staying blank.
check("A server the registry has not listed is named from the address", () => {
  const {page, document} = loadPage();
  page.PU.server = null;
  page.PU.servers = [];
  page.renderServers();
  const name = document.elements.get("server-current");
  equal(name.textContent, "http://127.0.0.1:8792", "name");
  if (name.textContent === "—") throw new Error("the page kept its placeholder");
});

check("The caret stays where a second server would appear", () => {
  const {page, document} = loadPage();
  page.PU.server = ONE;
  page.PU.servers = [ONE];
  page.renderServers();
  equal(document.elements.get("server-toggle").hidden, false, "caret hidden");
  equal(document.elements.get("server-list").hidden, true, "list hidden");
});

check("A second server fills the list and the caret opens it", () => {
  const {page, document} = loadPage();
  page.PU.server = ONE;
  page.PU.servers = [ONE, TWO];
  page.renderServers();
  const list = document.elements.get("server-list");
  equal(list.children.length, 2, "rows");
  equal(document.elements.get("server-current").disabled, false, "name disabled");
  equal(document.elements.get("server-toggle").disabled, false, "caret disabled");
});

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

// The page asked once at load and never again, which is why a turn finished during the visit
// was missing from a figure that was otherwise correct. The assertion is on the panel, not on
// the number of requests: a page that asks and then ignores the answer is the same bug.
check("A finished turn re-reads the total rather than keeping the one from load", async () => {
  const stub = async (url) => {
    if (url !== "/api/session") return [];
    return {success: true, usage_total: {turns: 7, rounds: 7, prompt_tokens: 10, completion_tokens: 2, cached_tokens: 0, priced_turns: 0}};
  };
  const {page, document} = loadPage(stub);
  page.renderUsageTotal({turns: 6, rounds: 6, prompt_tokens: 10, completion_tokens: 2, cached_tokens: 0, priced_turns: 0});
  const before = document.elements.get("usage-total").children[0].textContent;
  await page.refreshChainLength();
  const after = document.elements.get("usage-total").children[0].textContent;
  if (!before.includes("6 turns")) throw new Error(`before the turn ended: ${before}`);
  if (!after.includes("7 turns")) throw new Error(`after the turn ended: ${after}`);
});

Promise.all(pending).then(() => {
  if (failed > 0) {
    console.error(`\n${failed} case(s) failed`);
    process.exit(1);
  }
  console.log("\nall cases passed");
});
