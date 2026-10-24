#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

// A miniature of the page's context bar: load state.js and panel.js into a context that has the
// few elements they touch, then drive the two arrivals — a turn's counts, and the session's
// window — in both possible orders.

const web = path.join(__dirname, "..", "..", "web");

function makeElement(id) {
  return {
    id,
    textContent: "",
    title: "",
    className: "",
    style: {},
    children: [],
    replaceChildren() { this.children = []; },
    appendChild(child) { this.children.push(child); },
    addEventListener() {},
  };
}

const IDS = ["context-usage", "context-fill", "status-context", "usage-total"];

function load() {
  const elements = new Map(IDS.map((id) => [id, makeElement(id)]));
  const document = {
    elements,
    getElementById: (id) => elements.get(id) || null,
    createElement: (tag) => makeElement(tag),
  };
  const context = vm.createContext({
    console,
    document,
    URL,
    window: { location: { origin: "http://127.0.0.1:8792", protocol: "http:", hostname: "127.0.0.1", port: "8792", host: "127.0.0.1:8792" } },
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
  });
  // session.js is here for one function: `renderStatusBar` reads the thinking setting through
  // `settingValue`, which lives in that file. Loaded without it, every case here fails on
  // `settingValue is not defined` before it reaches an assertion — which is what the first run
  // of this file would have shown if a node had been present to run it.
  for (const file of ["blocks.js", "state.js", "session.js", "panel.js"]) {
    vm.runInContext(fs.readFileSync(path.join(web, file), "utf8"), context, { filename: file });
  }
  vm.runInContext(
    "globalThis.page = {get PU() { return PU; }, showUsage, renderContextBar, setBackendLabel, " +
    "setContextWindow, contextWindowFor};",
    context);
  return { page: context.page, elements };
}

function equal(actual, expected, what) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${what}: expected ${e}, got ${a}`);
}

function ok(condition, message) {
  if (!condition) throw new Error(message);
}

let failures = 0;
function check(name, body) {
  try {
    body();
    console.log(`ok   ${name}`);
  } catch (error) {
    failures += 1;
    console.error(`FAIL ${name}`);
    console.error(`  ${error.message}`);
  }
}

// A window stated for the running model fills the bar in proportion.
check("A stated window fills the bar", () => {
  const { page, elements } = load();
  page.setBackendLabel("deepseek", "deepseek-v4-pro", "https://api.deepseek.com/v1");
  page.setContextWindow("deepseek", "deepseek-v4-pro", 1000000);
  page.showUsage({ prompt_tokens: 250000, completion_tokens: 10, cached_tokens: 0 });
  equal(elements.get("context-fill").style.width, "25.0%", "fill width");
  equal(elements.get("context-fill").className, "", "fill class below the warn threshold");
});

// The warning thresholds are the ones the stylesheet names.
check("The bar warns and then colours as it fills", () => {
  const { page, elements } = load();
  page.setBackendLabel("deepseek", "m", "h");
  page.setContextWindow("deepseek", "m", 100);
  page.showUsage({ prompt_tokens: 70, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").className, "warn", "class at 70%");
  page.showUsage({ prompt_tokens: 95, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").className, "high", "class at 95%");
});

// A model that states nothing draws no fill and says why, rather than dividing by a guess.
check("A model with no window leaves the bar empty and says so", () => {
  const { page, elements } = load();
  page.setBackendLabel("nim", "nvidia/nemotron-3-ultra-550b-a55b", "https://integrate.api.nvidia.com/v1");
  page.setContextWindow("nim", "nvidia/nemotron-3-ultra-550b-a55b", 0);
  page.showUsage({ prompt_tokens: 5000, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").style.width, "0", "no fill");
  ok(elements.get("status-context").title.includes("no window"),
     `tooltip should explain the blank bar: ${elements.get("status-context").title}`);
});

// The two facts arrive on different clocks: a turn's counts come on the socket, the window comes
// with `/api/session`. Either order has to end with a filled bar, which is the bug a reload hits.
check("A window that arrives after the counts redraws the bar", () => {
  const { page, elements } = load();
  page.setBackendLabel("deepseek", "m", "h");
  page.showUsage({ prompt_tokens: 250000, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").style.width, "0", "blank before the window is known");
  page.setContextWindow("deepseek", "m", 1000000);
  page.renderContextBar();
  equal(elements.get("context-fill").style.width, "25.0%", "fill after the window lands");
});

check("Counts that arrive after the window redraw the bar", () => {
  const { page, elements } = load();
  page.setBackendLabel("deepseek", "m", "h");
  page.setContextWindow("deepseek", "m", 1000000);
  page.renderContextBar();
  equal(elements.get("context-fill").style.width, "0", "blank with no turn yet");
  page.showUsage({ prompt_tokens: 500000, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").style.width, "50.0%", "fill once a turn reports");
});

// The window belongs to the model, not the gateway: switching to another id must not inherit the
// previous one's figure, which would draw a fill against the wrong denominator.
check("A window is filed per model, not per gateway", () => {
  const { page, elements } = load();
  page.setBackendLabel("codebuddy", "big", "h");
  page.setContextWindow("codebuddy", "big", 1000000);
  page.showUsage({ prompt_tokens: 100000, completion_tokens: 1, cached_tokens: 0 });
  equal(elements.get("context-fill").style.width, "10.0%", "against the first model");
  page.setBackendLabel("codebuddy", "small", "h");
  equal(elements.get("context-fill").style.width, "0", "the second model has no window yet");
  page.setContextWindow("codebuddy", "small", 200000);
  page.renderContextBar();
  equal(elements.get("context-fill").style.width, "50.0%", "against its own");
});

// `0` and "absent" are different answers, and only the first should ever be stored as a figure.
check("A window of zero is forgotten rather than stored", () => {
  const { page } = load();
  page.setContextWindow("nim", "m", 0);
  equal(page.contextWindowFor("nim", "m"), null, "no window recorded");
  page.setContextWindow("nim", "m", 4096);
  equal(page.contextWindowFor("nim", "m"), 4096, "a real figure is kept");
  page.setContextWindow("nim", "m", null);
  equal(page.contextWindowFor("nim", "m"), null, "a later absent answer clears it");
});

if (failures > 0) {
  console.error(`\n${failures} case(s) failed`);
  process.exit(1);
}
console.log("\nall cases passed");
