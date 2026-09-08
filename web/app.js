"use strict";

const messagesEl = document.getElementById("messages");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("send");
const clearBtn = document.getElementById("clear-btn");
const agentSelect = document.getElementById("agent-select");

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
  currentAssistantEl.innerHTML = renderMarkdown(rawContent);
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
      const agent = data.agent_name || "?";
      const model = data.backend_model ? " · " + data.backend_model : "";
      document.getElementById("session-status")?.remove();
      const status = document.createElement("span");
      status.id = "session-status";
      status.textContent = `Agent: ${agent} · Backend: ${data.backend_type || "?"}${model}`;
      document.querySelector("header").appendChild(status);
      agentSelect.value = agent;
    }
  } catch (_) {}
}

async function loadHistory() {
  try {
    const res = await fetch("/api/history");
    const history = await res.json();
    if (!Array.isArray(history)) return;
    for (const msg of history) {
      if (msg.role === "system" || msg.role === "tool") continue;
      createMessage(msg.role, msg.content || "");
    }
  } catch (_) {}
}

async function loadAgents() {
  try {
    const res = await fetch("/api/agents");
    const data = await res.json();
    agentSelect.innerHTML = "";
    for (const agent of data.agents || []) {
      const opt = document.createElement("option");
      opt.value = agent.name;
      opt.textContent = agent.name + (agent.description ? " (" + agent.description + ")" : "");
      agentSelect.appendChild(opt);
    }
    agentSelect.addEventListener("change", async () => {
      const name = agentSelect.value;
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
          createSystemMessage("Switch failed: " + (data.error || "unknown"));
        }
      } catch (e) {
        createSystemMessage("Switch failed: " + e.message);
      }
    });
  } catch (_) {}
}

async function clearChat() {
  clearBtn.disabled = true;
  try {
    const res = await fetch("/api/clear", { method: "POST" });
    const data = await res.json();
    messagesEl.innerHTML = "";
    createSystemMessage(data.success ? "Conversation cleared." : "Clear failed: " + (data.error || "unknown"));
    await loadSession();
    await loadHistory();
  } catch (e) {
    messagesEl.innerHTML = "";
    createSystemMessage("Clear failed: " + e.message);
  } finally {
    clearBtn.disabled = false;
  }
}

messagesEl.addEventListener("scroll", () => {
  const threshold = 10;
  const distance = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight;
  isAtBottom = distance < threshold;
});

sendBtn.addEventListener("click", sendMessage);
clearBtn.addEventListener("click", clearChat);
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
loadSession();
loadHistory();
loadAgents();
