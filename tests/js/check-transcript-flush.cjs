#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only

//
// The live bubble and the frames that finish it, checked as the page runs them.
//
// A redraw is deferred to the next animation frame so a fast stream costs one paint rather than
// one per chunk. The bug that deferral carries is the one this file exists for: `done` arrives in
// the same tick as the last chunk, and ending the turn used to throw the bubble away before the
// queued frame ran. The frame then found no bubble to draw into, returned, and the answer stayed
// on screen as a prefix of what the store held — which is why a reload showed the rest. Nothing
// was lost on the server's side, so nothing looked broken from there.
//
// The other half is a tab nobody is looking at: a browser does not paint a hidden tab, so a redraw
// it is holding waits for the reader to come back and the reply stops at the point they left it.
//
// The stub is a frame queue and the two things a bubble answers: somewhere to put child nodes,
// and whether it is still the live one.
//

const {readFileSync} = require("node:fs");
const {join} = require("node:path");
const vm = require("node:vm");

const web = join(__dirname, "..", "..", "web");

// A frame queue that runs only when a case says so, which is what lets a case hold the page in
// the state the bug lives in: "queued but not yet drawn".
function makeFrames() {
  const queued = new Map();
  let next = 1;
  return {
    requestAnimationFrame(callback) {
      const id = next++;
      queued.set(id, callback);
      return id;
    },
    cancelAnimationFrame(id) {
      queued.delete(id);
    },
    // The browser's next paint: every callback queued now runs.
    pump() {
      const due = Array.from(queued.values());
      queued.clear();
      for (const callback of due) callback();
    },
    pending() {
      return queued.size;
    },
  };
}

function makeContainer() {
  const children = [];
  return {
    childNodes: children,
    insertBefore(node, before) {
      const at = before === null ? children.length : children.indexOf(before);
      children.splice(at < 0 ? children.length : at, 0, node);
    },
    appendChild(node) {
      children.push(node);
    },
    removeChild(node) {
      const at = children.indexOf(node);
      if (at >= 0) children.splice(at, 1);
    },
    get lastChild() {
      return children.length ? children[children.length - 1] : null;
    },
  };
}

function loadPage() {
  const frames = makeFrames();
  const listeners = new Map();
  const container = makeContainer();

  const document = {
    hidden: false,
    addEventListener(type, handler) {
      listeners.set(type, handler);
    },
    createElement: () => ({ className: "", style: {} }),
  };

  const context = vm.createContext({
    console,
    document,
    window: { location: { origin: "http://127.0.0.1:8792", hostname: "127.0.0.1", port: "8792" } },
    requestAnimationFrame: frames.requestAnimationFrame,
    cancelAnimationFrame: frames.cancelAnimationFrame,
    setTimeout,
    clearTimeout,
  });

  for (const file of ["blocks.js", "state.js", "transcript.js"]) {
    vm.runInContext(readFileSync(join(web, file), "utf8"), context, {filename: file});
  }

  // The bubble and the block elements belong to the browser. What this file asks is whether they
  // were asked to draw at all, so they are replaced with a record of exactly that.
  vm.runInContext(`
    createMessage = function () {
      return {
        className: "",
        classList: {add() {}, remove() {}},
        removed: false,
        remove() { this.removed = true; },
        querySelector() { return globalThis.container; },
      };
    };
    createBlockNode = function () {
      return {
        drawn: 0,
        sync() { this.drawn += 1; },
      };
    };
    isAtBottom = () => true;
    scrollToBottom = () => { globalThis.scrolls += 1; };
  `, context);

  context.container = container;
  context.scrolls = 0;
  vm.runInContext(
      "globalThis.page = {get PU() { return PU; }, startAssistantMessage, finishAssistantMessage, " +
      "removeCurrentAssistantMessage, handleChunk, handleThinking, flushAssistantRender, " +
      "discardAssistantRender};",
      context);
  return {context, page: context.page, frames, listeners, container, document};
}

let failed = 0;

function check(name, body) {
  try {
    body();
    console.log(`ok   ${name}`);
  } catch (error) {
    failed += 1;
    console.error(`FAIL ${name}`);
    console.error(`  ${error.message}`);
  }
}

function equal(actual, expected, what) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

// The deferral itself: a chunk puts the text in the block list and asks for one paint, and the
// bubble has not been drawn into yet. Every case below starts from this state.
check("A chunk queues its redraw rather than drawing at once", () => {
  const {page, frames, container} = loadPage();
  page.startAssistantMessage();
  page.handleChunk({text: "hello"});
  equal(frames.pending(), 1, "frames queued");
  equal(container.childNodes.length, 0, "drawn before the frame");
});

// The bug, as the reader meets it. A chunk and the `done` that ends the turn arrive in one tick,
// so the frame carrying the last chunk is still queued when the turn ends. Ending the turn used
// to drop the bubble first; the frame then found nothing to draw into and returned.
check("Ending a turn draws the block its queued frame was holding", () => {
  const {page, frames, container} = loadPage();
  page.startAssistantMessage();
  page.handleChunk({text: "abcdef"});
  equal(frames.pending(), 1, "one frame queued");
  equal(container.childNodes.length, 0, "drawn before the frame");

  page.finishAssistantMessage();

  equal(frames.pending(), 0, "the queued frame was taken rather than left to run");
  if (container.childNodes.length !== 1) {
    throw new Error(`the bubble holds ${container.childNodes.length} blocks, not the one written`);
  }
  equal(page.PU.currentAssistantEl, null, "the bubble stopped being the live one");
});

// The same turn with a paint in the middle, which is the ordinary case: the answer is drawn while
// streaming and the last chunk is the only one that could be missed.
check("A turn drawn as it streams still ends with its last chunk on the page", () => {
  const {page, frames, container} = loadPage();
  page.startAssistantMessage();
  page.handleChunk({text: "the first half, "});
  frames.pump();
  equal(container.childNodes.length, 1, "the first half is on the page");

  page.handleChunk({text: "and the second."});
  page.finishAssistantMessage();

  equal(frames.pending(), 0, "nothing left queued");
  const block = page.PU.currentAssistantBlocks[0];
  equal(block === undefined, true, "the block list was released with the bubble");
  equal(container.childNodes[0].drawn >= 1, true, "the block was drawn at least once");
});

// A bubble that is being thrown away has nothing to draw into, so its frame is cancelled rather
// than run against an element that has left the page.
check("Discarding a bubble cancels the frame that would have drawn into it", () => {
  const {page, frames} = loadPage();
  page.startAssistantMessage();
  page.handleChunk({text: "half a reply"});
  equal(frames.pending(), 1, "one frame queued");

  page.removeCurrentAssistantMessage();

  equal(frames.pending(), 0, "nothing left queued");
  equal(page.PU.currentAssistantEl, null, "the bubble is gone");
});

// A hidden tab is not painted, so a redraw it holds is delivered only when the reader returns. The
// page draws what it owes as it goes out of sight, so coming back shows the whole answer.
check("A tab going to the background draws what it was holding", () => {
  const {page, frames, listeners, container, document} = loadPage();
  page.startAssistantMessage();
  page.handleChunk({text: "written while visible"});
  equal(frames.pending(), 1, "one frame queued");

  document.hidden = true;
  const onChange = listeners.get("visibilitychange");
  if (!onChange) throw new Error("nothing listens for the tab being hidden");
  onChange();

  equal(frames.pending(), 0, "the held frame was drawn and taken");
  equal(container.childNodes.length, 1, "the reply is on the page");

  // The marker has to be clear, or the next chunk queues nothing and the page stops drawing for
  // the rest of the visit.
  page.handleChunk({text: " and after"});
  equal(frames.pending(), 1, "a later chunk still queues a frame");
  frames.pump();
  equal(page.PU.currentAssistantBlocks[0].content, "written while visible and after", "content");
});

// Reasoning is not the answer arriving, but it is drawn through the same queue and finished by
// the same gesture: a page that froze while thinking shows a spinner over nothing.
check("Reasoning is held by the same queue", () => {
  const {page, frames, container} = loadPage();
  page.startAssistantMessage();
  page.handleThinking({text: "weighing it"});
  equal(frames.pending(), 1, "one frame queued");

  page.finishAssistantMessage();

  equal(frames.pending(), 0, "the queued frame was taken");
  equal(container.childNodes.length, 1, "the reasoning block is on the page");
});

if (failed > 0) {
  console.error(`\n${failed} case(s) failed`);
  process.exit(1);
}
console.log("\nall cases passed");
