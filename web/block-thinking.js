// SPDX-License-Identifier: GPL-3.0-only
// The reasoning and answer blocks, each owning the element it draws and the facts it
// re-reads. `block-node.js` decides which one a block gets.

"use strict";

function renderMarkdown(text) {
  return DOMPurify.sanitize(marked.parse(text));
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

  toggled() {
    return this._block.collapsed;
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
