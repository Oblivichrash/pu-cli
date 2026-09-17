"use strict";

const messagesEl = document.getElementById("messages");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("send");
let agentSelect = document.getElementById("agent-select");
let agentChangeHandler = null;

let ws = null;
let isStreaming = false;
let isAtBottom = true;

const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  TEXT: "text",
};

let currentAssistantBlocks = [];
let currentAssistantEl = null;

const markedOptions = {
  breaks: true,
  highlight: (code, lang) => {
    if (lang && hljs.getLanguage(lang)) {
      try { return hljs.highlight(code, { language: lang }).value; } catch (_) {}
    }
    try { return hljs.highlightAuto(code).value; } catch (_) {}
    return code;
  },
};
marked.setOptions(markedOptions);

function createSystemMessage(text) {
  const el = document.createElement("div");
  el.className = "msg system";
  el.textContent = text;
  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
}

function createMessage(role, blocks) {
  const el = document.createElement("div");
  el.className = "msg " + role;

  const label = document.createElement("span");
  label.className = "role";
  label.textContent = role === "user" ? "You" : "Assistant";
  el.appendChild(label);

  const container = document.createElement("div");
  container.className = "blocks-container";
  el.appendChild(container);

  if (blocks) {
    renderBlocks(blocks, container);
  }

  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
}

function renderBlocks(blocks, container) {
  container.innerHTML = "";
  for (const block of blocks) {
    const el = renderBlock(block);
    if (el) container.appendChild(el);
  }
}

function renderBlock(block) {
  switch (block.type) {
    case BLOCK_TYPES.THINKING:
      return renderThinkingBlock(block);
    case BLOCK_TYPES.TOOL_CALL:
      return renderToolBlock(block);
    case BLOCK_TYPES.TEXT:
      return renderTextBlock(block);
    default:
      return null;
  }
}

function renderThinkingBlock(block) {
  const wrapper = document.createElement("div");
  wrapper.className = "block-thinking";

  const header = document.createElement("div");
  header.className = "block-header";
  header.textContent = "💭 Thinking";
  header.onclick = () => toggleBlock(header);

  const icon = document.createElement("span");
  icon.className = "toggle-icon";
  icon.textContent = block.collapsed ? "▶" : "▼";
  header.appendChild(icon);

  const body = document.createElement("div");
  body.className = "block-body" + (block.collapsed ? " collapsed" : "");
  body.textContent = block.content;

  wrapper.appendChild(header);
  wrapper.appendChild(body);
  return wrapper;
}

function renderToolBlock(block) {
  const wrapper = document.createElement("div");
  wrapper.className = "block-tool";
  wrapper.dataset.toolId = block.id || "";

  const header = document.createElement("div");
  header.className = "block-header";
  header.textContent = "🔧 " + (block.name || "unknown");
  header.onclick = () => toggleBlock(header);

  const statusSpan = document.createElement("span");
  statusSpan.className = "tool-status";
  if (block.status === "running") {
    statusSpan.textContent = " ⏳ Running...";
  } else if (block.error) {
    statusSpan.textContent = " ❌ Failed";
  } else {
    statusSpan.textContent = " ✅ Done";
  }
  header.appendChild(statusSpan);

  const icon = document.createElement("span");
  icon.className = "toggle-icon";
  icon.textContent = block.collapsed ? "▶" : "▼";
  header.appendChild(icon);

  const body = document.createElement("div");
  body.className = "block-body" + (block.collapsed ? " collapsed" : "");

  const argsDiv = document.createElement("div");
  argsDiv.className = "tool-args";
  argsDiv.innerHTML = "<strong>Arguments</strong><pre>" +
    JSON.stringify(block.args, null, 2) + "</pre>";
  body.appendChild(argsDiv);

  if (block.status === "done") {
    const resultDiv = document.createElement("div");
    resultDiv.className = "tool-result";
    if (block.error) {
      resultDiv.innerHTML = "<strong>Error</strong><pre class=\"tool-error\">" +
        block.error + "</pre>";
    } else {
      const output = block.output || "(no output)";
      resultDiv.innerHTML = "<strong>Output</strong><pre>" + output + "</pre>";
    }
    body.appendChild(resultDiv);
  }

  wrapper.appendChild(header);
  wrapper.appendChild(body);
  return wrapper;
}

function renderTextBlock(block) {
  const div = document.createElement("div");
  div.className = "block-text";
  div.innerHTML = renderMarkdown(block.content);
  return div;
}

function renderMarkdown(text) {
  return DOMPurify.sanitize(marked.parse(text));
}

function toggleBlock(headerEl) {
  const body = headerEl.parentElement.querySelector(".block-body");
  const icon = headerEl.querySelector(".toggle-icon");
  if (!body) return;
  const isCollapsed = body.classList.toggle("collapsed");
  if (icon) icon.textContent = isCollapsed ? "▶" : "▼";
}

function startAssistantMessage() {
  currentAssistantBlocks = [];
  currentAssistantEl = createMessage("assistant", currentAssistantBlocks);
  currentAssistantEl.classList.add("typing");
}

function finishAssistantMessage() {
  if (currentAssistantEl) {
    currentAssistantEl.classList.remove("typing");
    currentAssistantEl = null;
    currentAssistantBlocks = [];
  }
}

function removeCurrentAssistantMessage() {
  if (currentAssistantEl) {
    currentAssistantEl.remove();
    currentAssistantEl = null;
    currentAssistantBlocks = [];
  }
}

function updateCurrentAssistantBlocks() {
  if (!currentAssistantEl) return;
  const container = currentAssistantEl.querySelector(".blocks-container");
  if (container) {
    renderBlocks(currentAssistantBlocks, container);
    if (isAtBottom) messagesEl.scrollTop = messagesEl.scrollHeight;
  }
}

function handleToolStart(payload) {
  const block = {
    type: BLOCK_TYPES.TOOL_CALL,
    id: payload.id,
    name: payload.name,
    args: payload.args,
    output: "",
    error: "",
    status: "running",
    collapsed: true,
  };
  currentAssistantBlocks.push(block);
  updateCurrentAssistantBlocks();
}

function handleToolEnd(payload) {
  const block = currentAssistantBlocks.find(b =>
    b.type === BLOCK_TYPES.TOOL_CALL && b.id === payload.id
  );
  if (block) {
    block.output = payload.output || "";
    block.error = payload.error || "";
    block.status = "done";
    updateCurrentAssistantBlocks();
  }
}

function handleChunk(payload) {
  const text = payload.text || "";
  let lastBlock = currentAssistantBlocks[currentAssistantBlocks.length - 1];
  if (!lastBlock || lastBlock.type !== BLOCK_TYPES.TEXT) {
    lastBlock = { type: BLOCK_TYPES.TEXT, content: "" };
    currentAssistantBlocks.push(lastBlock);
  }
  lastBlock.content += text;
  updateCurrentAssistantBlocks();
}

function handleDone() {
  finishAssistantMessage();
  setSendButtonState(false);
}

function handleError(payload) {
  const errMsg = payload.text || "Unknown error";
  if (currentAssistantEl) {
    currentAssistantEl.classList.remove("typing");
    currentAssistantEl.className = "msg error";
    currentAssistantEl.innerHTML = errMsg;
    currentAssistantEl = null;
    currentAssistantBlocks = [];
  } else {
    createSystemMessage("Error: " + errMsg);
  }
  setSendButtonState(false);
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const url = `${protocol}//${window.location.host}/ws`;
  ws = new WebSocket(url);

  ws.onopen = () => {
    createSystemMessage("Connected to server.");
  };

  ws.onmessage = (event) => {
    let data;
    try { data = JSON.parse(event.data); } catch (_) { return; }

    switch (data.type) {
      case "tool_start":
        handleToolStart(data.payload);
        break;
      case "tool_end":
        handleToolEnd(data.payload);
        break;
      case "chunk":
        handleChunk(data.payload);
        break;
      case "done":
        handleDone();
        break;
      case "error":
        handleError(data.payload);
        break;
      default:
        break;
    }
  };

  ws.onclose = () => {
    createSystemMessage("Disconnected from server.");
    if (isStreaming) {
      removeCurrentAssistantMessage();
      setSendButtonState(false);
    }
  };

  ws.onerror = () => {
    createSystemMessage("WebSocket error.");
    if (isStreaming) {
      removeCurrentAssistantMessage();
      setSendButtonState(false);
    }
  };
}

function setSendButtonState(streaming) {
  isStreaming = streaming;
  sendBtn.textContent = streaming ? "Stop" : "Send";
  sendBtn.disabled = false;
}

function sendMessage() {
  if (isStreaming) {
    ws.send(JSON.stringify({ type: "cancel" }));
    ws.close();
    removeCurrentAssistantMessage();
    setSendButtonState(false);
    return;
  }

  const text = inputEl.value.trim();
  if (!text) return;
  inputEl.value = "";
  inputEl.style.height = "auto";

  createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: text }]);
  startAssistantMessage();
  setSendButtonState(true);

  ws.send(JSON.stringify({ type: "run", payload: { text } }));
}

async function loadHistory() {
  try {
    const res = await fetch("/api/history");
    const history = await res.json();
    if (!Array.isArray(history)) return;

    for (const msg of history) {
      const role = msg.role;
      const content = msg.content || "";

      if (role === "system") {
        createSystemMessage(content);
        continue;
      }

      if (role === "tool") {
        continue;
      }

      if (role === "assistant") {
        const blocks = [];
        const toolCalls = Array.isArray(msg.tool_calls) ? msg.tool_calls : [];

        if (msg.reasoning_content) {
          blocks.push({
            type: BLOCK_TYPES.THINKING,
            content: msg.reasoning_content,
            collapsed: true,
          });
        }

        for (const tc of toolCalls) {
          const fn = tc.function || {};
          const args = fn.arguments || {};
          blocks.push({
            type: BLOCK_TYPES.TOOL_CALL,
            id: tc.id || "unknown",
            name: fn.name || "unknown",
            args: typeof args === "string" ? JSON.parse(args) : args,
            output: "",
            error: "",
            status: "done",
            collapsed: true,
          });
        }

        if (content) {
          blocks.push({ type: BLOCK_TYPES.TEXT, content });
        }

        createMessage("assistant", blocks);
        continue;
      }

      createMessage("user", [{ type: BLOCK_TYPES.TEXT, content }]);
    }
  } catch (_) {}
}

async function loadSession() {
  try {
    const res = await fetch("/api/session");
    const data = await res.json();
    if (data.ok) {
      const model = data.backend_model ? " · " + data.backend_model : "";
      document.getElementById("session-status")?.remove();
      const status = document.createElement("span");
      status.id = "session-status";
      status.textContent = `Backend: ${data.backend_type || "?"}${model}`;
      document.querySelector("header").appendChild(status);
      if (data.agent_name) {
        agentSelect.value = data.agent_name;
      }
    }
  } catch (_) {}
}

async function loadAgents() {
  try {
    const res = await fetch("/api/agents");
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    if (agentChangeHandler) {
      agentSelect.removeEventListener("change", agentChangeHandler);
    }

    agentSelect.innerHTML = "";
    if (data.agents && data.agents.length > 0) {
      for (const agent of data.agents) {
        const opt = document.createElement("option");
        opt.value = agent.name;
        opt.textContent = agent.name + (agent.description ? " (" + agent.description + ")" : "");
        agentSelect.appendChild(opt);
      }
    } else {
      const opt = document.createElement("option");
      opt.value = "";
      opt.textContent = "No agents available";
      agentSelect.appendChild(opt);
    }

    agentChangeHandler = async () => {
      const name = agentSelect.value;
      if (!name) return;
      try {
        const res = await fetch("/api/agent/switch", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ agent_name: name })
        });
        const data = await res.json();
        if (data.success) {
          messagesEl.innerHTML = "";
          createSystemMessage("Switched to agent: " + name);
          await loadSession();
          await loadHistory();
        } else {
          createSystemMessage("Switch failed: " + (data.error || "unknown error"));
        }
      } catch (e) {
        createSystemMessage("Switch failed: " + e.message);
      }
    };
    agentSelect.addEventListener("change", agentChangeHandler);
  } catch (e) {
    createSystemMessage("Failed to load agents: " + e.message);
    console.error("loadAgents error:", e);
  }
}

messagesEl.addEventListener("scroll", () => {
  const threshold = 10;
  const distance = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight;
  isAtBottom = distance < threshold;
});

sendBtn.addEventListener("click", sendMessage);
inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    if (!isStreaming) sendMessage();
  }
});
inputEl.addEventListener("input", () => {
  inputEl.style.height = "auto";
});

connectWebSocket();

(async () => {
  await loadAgents();
  await loadSession();
  await loadHistory();
})();
