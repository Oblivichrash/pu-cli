#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only

//
// groupHistory() turned into what a page draws, checked against what a reader saw.
//
// The page's modules are browser globals loaded by index.html, so this runs them the way
// the page does: in one context, in the order the page names them. The case file is the
// contract — a stored turn's JSON in, the bubbles out — and it is the only thing that has
// to change when a shape is added.
//
// No DOM is involved: groupHistory is a fold over the history array, and the moment it
// needs a document this check stops being able to run without one.
//

const {readFileSync} = require("node:fs");
const {dirname, join} = require("node:path");
const vm = require("node:vm");

const here = __dirname;
const web = join(here, "..", "web");
const casesPath = join(here, "..", "fixtures", "group_history.json");

// state.js declares the block types; history.js reads them. Both are top-level consts, so
// they live in the context the script is run in rather than on an object it hands back.
const context = vm.createContext({console});
for (const file of ["state.js", "history.js"]) {
  const source = readFileSync(join(web, file), "utf8");
  vm.runInContext(source, context, {filename: file});
}

const groupHistory = context.groupHistory;
if (typeof groupHistory !== "function") {
  console.error("history.js did not define groupHistory");
  process.exit(1);
}

const cases = JSON.parse(readFileSync(casesPath, "utf8")).cases;

// Field order is not part of the contract, so both sides are compared with sorted keys. A
// missing or extra field is still a difference.
function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") {
    const out = {};
    for (const key of Object.keys(value).sort()) out[key] = canonical(value[key]);
    return out;
  }
  return value;
}

let failed = 0;
for (const testCase of cases) {
  // The page's own shapes carry no undefined members, and JSON round-tripping drops any a
  // case file left out, so both sides start from what JSON can express.
  const actual = canonical(JSON.parse(JSON.stringify(groupHistory(testCase.history))));
  const expected = canonical(testCase.expected);
  if (JSON.stringify(actual) === JSON.stringify(expected)) {
    console.log(`ok   ${testCase.name}`);
    continue;
  }
  failed += 1;
  console.error(`FAIL ${testCase.name}`);
  console.error(`  expected ${JSON.stringify(expected)}`);
  console.error(`  actual   ${JSON.stringify(actual)}`);
}

if (failed > 0) {
  console.error(`\n${failed} of ${cases.length} case(s) failed`);
  process.exit(1);
}
console.log(`\n${cases.length} case(s) passed`);
