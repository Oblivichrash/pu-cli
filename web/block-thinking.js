// SPDX-License-Identifier: GPL-3.0-only
// The transcript's leaf widgets, each owning the element it draws and the handful of
// facts it is expected to re-read. `block-node.js` decides which one a block gets.

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
    header.className = "block-header";
    header.textContent = "💭 Thinking";
    header.onclick = () => toggleBlock(header);

    const icon = document.createElement("span");
    icon.className = "toggle-icon";
    header.appendChild(icon);

    const body = document.createElement("div");
    body.className = "block-body";

    this.append(header, body);
    if (this._block) this.sync(this._block);
  }

  sync(block) {
    this._block = block;
    const body = this.querySelector(".block-body");
    const icon = this.querySelector(".toggle-icon");
    if (!body) return;
    body.textContent = block.content;
    body.classList.toggle("collapsed", !!block.collapsed);
    icon.textContent = block.collapsed ? "▶" : "▼";
  }

  toggle() {
    this._block.collapsed = !this._block.collapsed;
    this.sync(this._block);
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
