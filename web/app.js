"use strict";

const messagesEl = document.getElementById("messages");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("send");
let agentSelect = document.getElementById("agent-select");
let agentChangeHandler = null;

let ws = null;
let isStreaming = false;
let currentAssistantEl = null;
let rawContent = "";
let isAtBottom = true;

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

function createMessage(role, text = "") {
  const el = document.createElement("div");
  el.className = "msg " + role;
  const label = document.createElement("span");
  label.className = "role";
  label.textContent = role === "user" ? "You" : "Assistant";
  el.appendChild(label);
  if (text) {
    const content = document.createTextNode(text);
    el.appendChild(content);
  }
  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
}

function createSystemMessage(text) {
  const el = document.createElement("div");
  el.className = "msg system";
  el.textContent = text;
  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
}

function renderMarkdown(text) {
  return DOMPurify.sanitize(marked.parse(text));
}

function updateAssistantBubble(text) {
  if (!currentAssistantEl) {
    currentAssistantEl = createMessage("assistant", "");
    rawContent = "";
  }
  rawContent += text;
  const normalized = rawContent.replace(/\n{2,}/g, '\n');
  currentAssistantEl.innerHTML = renderMarkdown(normalized);
  if (isAtBottom) messagesEl.scrollTop = messagesEl.scrollHeight;
}

function finishAssistantBubble() {
  if (currentAssistantEl) {
    currentAssistantEl.classList.remove("typing");
    currentAssistantEl = null;
    rawContent = "";
  }
}

function removeCurrentAssistantBubble() {
  if (currentAssistantEl) {
    currentAssistantEl.remove();
    currentAssistantEl = null;
    rawContent = "";
  }
}

function setSendButtonState(streaming) {
  isStreaming = streaming;
  sendBtn.textContent = streaming ? "Stop" : "Send";
  sendBtn.disabled = false;
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
    const type = data.type;
    if (type === "chunk") {
      const text = data.payload?.text || "";
      if (text) updateAssistantBubble(text);
    } else if (type === "done") {
      finishAssistantBubble();
      setSendButtonState(false);
    } else if (type === "error") {
      const errMsg = data.payload?.text || "Unknown error";
      if (currentAssistantEl) {
        currentAssistantEl.classList.remove("typing");
        currentAssistantEl.className = "msg error";
        currentAssistantEl.innerHTML = errMsg;
        currentAssistantEl = null;
        rawContent = "";
      } else {
        createMessage("error", errMsg);
      }
      setSendButtonState(false);
    }
  };

  ws.onclose = () => {
    createSystemMessage("Disconnected from server.");
    if (isStreaming) {
      removeCurrentAssistantBubble();
      setSendButtonState(false);
    }
  };

  ws.onerror = () => {
    createSystemMessage("WebSocket error.");
    if (isStreaming) {
      removeCurrentAssistantBubble();
      setSendButtonState(false);
    }
  };
}

function sendMessage() {
  if (isStreaming) {
    ws.send(JSON.stringify({ type: "cancel" }));
    ws.close();
    removeCurrentAssistantBubble();
    setSendButtonState(false);
    return;
  }

  const text = inputEl.value.trim();
  if (!text) return;
  inputEl.value = "";
  inputEl.style.height = "auto";

  createMessage("user", text);
  currentAssistantEl = createMessage("assistant", "");
  currentAssistantEl.classList.add("typing");
  rawContent = "";
  setSendButtonState(true);

  ws.send(JSON.stringify({ type: "run", payload: { text } }));
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
      } else if (role === "tool") {
        const toolLabel = msg.tool_name ? `Tool: ${msg.tool_name}` : "Tool";
        const el = document.createElement("div");
        el.className = "msg assistant";
        const label = document.createElement("span");
        label.className = "role";
        label.textContent = toolLabel;
        el.appendChild(label);
        const textNode = document.createTextNode(content);
        el.appendChild(textNode);
        messagesEl.appendChild(el);
        messagesEl.scrollTop = messagesEl.scrollHeight;
      } else if (role === "assistant") {
        const el = document.createElement("div");
        el.className = "msg assistant";
        const label = document.createElement("span");
        label.className = "role";
        label.textContent = "Assistant";
        el.appendChild(label);

        if (content) {
          const contentDiv = document.createElement("div");
          contentDiv.innerHTML = renderMarkdown(content);
          el.appendChild(contentDiv);
        }

        if (msg.tool_calls_json) {
          try {
            const toolCalls = JSON.parse(msg.tool_calls_json);
            if (Array.isArray(toolCalls) && toolCalls.length > 0) {
              const details = document.createElement("div");
              details.className = "tool-call-details";
              details.style.cssText = "margin-top: 8px; padding: 6px 10px; background: var(--paper); border-radius: 2px; font-size: 13px; border-left: 3px solid var(--accent);";
              for (const tc of toolCalls) {
                const fn = tc.function || {};
                const name = fn.name || "unknown";
                const args = fn.arguments || {};
                const argsStr = typeof args === "string" ? args : JSON.stringify(args, null, 2);
                const item = document.createElement("div");
                item.innerHTML = `<strong>🔧 ${name}</strong><pre style="margin: 4px 0 0 0; white-space: pre-wrap; background: var(--paper); padding: 4px 8px; border-radius: 2px;">${argsStr}</pre>`;
                details.appendChild(item);
              }
              el.appendChild(details);
            }
          } catch (e) {
            // ignore
          }
        }

        messagesEl.appendChild(el);
        messagesEl.scrollTop = messagesEl.scrollHeight;
      } else {
        // user
        createMessage(role, content);
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
