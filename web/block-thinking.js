// SPDX-License-Identifier: GPL-3.0-only
// The reasoning and answer blocks, each owning the element it draws and the facts it
// re-reads. `block-node.js` decides which one a block gets.

"use strict";

// The one way any block turns prose into DOM. `marked` marks a fenced block's language on the
// code element rather than on the `pre`, and a reader who is about to copy a command wants that
// name on the block itself: it says what the lines are before they are read. A fence with no
// language gets no name, since inventing one would be a claim the source never made.
function renderMarkdown(text) {
  const html = marked.parse(text);
  const holder = document.createElement("div");
  holder.innerHTML = DOMPurify.sanitize(html);
  for (const pre of holder.querySelectorAll("pre")) {
    const code = pre.firstElementChild;
    const language = code && code.className.startsWith("language-")
      ? code.className.slice("language-".length)
      : "";
    if (!language) continue;
    const name = document.createElement("span");
    name.className = "code-lang";
    name.textContent = language;
    pre.classList.add("has-lang");
    pre.prepend(name);
  }
  return holder.innerHTML;
}

class ThinkingBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-thinking";

    const header = document.createElement("div");
    header.textContent = "💭 Thinking";
    buildCollapsible(this, header);

    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    this.body.textContent = block.content;
    syncCollapsible(this, block);
  }

  toggle() {
    toggleSelf(this);
  }
}

class TextBlock extends HTMLElement {
  connectedCallback() {
    if (this.dataset.built) return;
    this.dataset.built = "1";
    this.className = "block-text";
    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    this.innerHTML = renderMarkdown(block.content);
  }
}

customElements.define("pu-thinking", ThinkingBlock);
customElements.define("pu-text", TextBlock);
