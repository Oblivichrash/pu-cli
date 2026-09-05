"use strict";

const toolCallMap = new Map(); // Maps tool_call_id to the tool result content.

const messagesEl = document.getElementById("messages");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("send");
const clearBtn = document.getElementById("clear-btn");
const statusEl = document.getElementById("session-status");
const agentSelect = document.getElementById("agent-select");

// State for cancellation
let currentRequestId = null;
let currentController = null;

function addMessage(role, text, extra) {
  const el = document.createElement("div");
  el.className = "msg " + (role === "user" ? "user" : (role === "error" ? "error" : "assistant"));
  const label = document.createElement("span");
  label.className = "role";
  label.textContent = role === "user" ? "You" : "Assistant";
  el.appendChild(label);

  const body = document.createTextNode(text == null ? "" : String(text));
  el.appendChild(body);

  const toolCalls = extra && Array.isArray(extra.tool_calls) ? extra.tool_calls : null;
  if (role === "assistant" && toolCalls && toolCalls.length > 0) {
    const toggle = document.createElement("button");
    toggle.className = "toggle-details";
    toggle.textContent = "▶ 查看工具调用";
    const details = document.createElement("div");
    details.className = "details";
    details.hidden = true;

    for (const call of toolCalls) {
      const fn = (call && call.function) ? call.function : {};
      const name = fn.name || "(unknown tool)";
      let argsText = "";
      try {
        const args = fn.arguments;
        if (args === undefined || args === null) {
          argsText = "{}";
        } else if (typeof args === "string") {
          argsText = JSON.stringify(JSON.parse(args), null, 2);
        } else {
          argsText = JSON.stringify(args, null, 2);
        }
      } catch (e) {
        argsText = String(fn.arguments || "{}");
      }

      const callId = call.id || "";
      const resultText = callId && toolCallMap.has(callId) ? toolCallMap.get(callId) : "No result";

      const detail = document.createElement("div");
      detail.className = "tool-call-detail";

      const nameEl = document.createElement("div");
      nameEl.className = "tool-name";
      nameEl.textContent = name;

      const argsEl = document.createElement("pre");
      argsEl.className = "tool-arguments";
      argsEl.textContent = argsText;

      const resultEl = document.createElement("div");
      resultEl.className = "tool-result";
      resultEl.textContent = "Result: " + resultText;

      detail.appendChild(nameEl);
      detail.appendChild(argsEl);
      detail.appendChild(resultEl);
      details.appendChild(detail);
    }

    toggle.addEventListener("click", () => {
      const hidden = details.hidden;
      details.hidden = !hidden;
      toggle.textContent = hidden ? "▼ 收起工具调用" : "▶ 查看工具调用";
    });

    el.appendChild(toggle);
    el.appendChild(details);
  }

  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
}

function addSystem(text) {
  const el = document.createElement("div");
  el.className = "msg system";
  el.textContent = text;
  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
}

async function loadSession() {
  try {
    const res = await fetch("/api/session");
    const data = await res.json();
    if (data.ok) {
      const model = data.backend_model ? " · " + data.backend_model : "";
      statusEl.textContent = "Agent: " + (data.agent_name || "?") +
        " · Backend: " + (data.backend_type || "?") + model;
      if (data.agent_name) {
        agentSelect.value = data.agent_name;
      }
    } else {
      statusEl.textContent = "Session unavailable: " + (data.error || "unknown");
    }
  } catch (e) {
    statusEl.textContent = "Session unavailable";
  }
}

async function loadHistory() {
  try {
    const res = await fetch("/api/history");
    const history = await res.json();
    if (Array.isArray(history)) {
      toolCallMap.clear();

      for (const msg of history) {
        if (msg.role === "tool" && msg.tool_call_id) {
          toolCallMap.set(msg.tool_call_id, msg.content);
        }
      }

      for (const msg of history) {
        if (msg.role === "system" || msg.role === "tool") continue;

        let extra = {};
        if (msg.role === "assistant" && msg.tool_calls_json) {
          try {
            const parsed = JSON.parse(msg.tool_calls_json);
            if (Array.isArray(parsed) && parsed.length > 0) {
              extra.tool_calls = parsed;
            }
          } catch (e) {
            console.warn("Failed to parse tool_calls_json:", e);
          }
        }
        addMessage(msg.role, msg.content, extra);
      }
    }
  } catch (e) {
    console.warn("Failed to load history:", e);
  }
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
    agentSelect.addEventListener("change", async function() {
      const name = this.value;
      try {
        const res = await fetch("/api/agent/switch", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ agent_name: name })
        });
        const data = await res.json();
        if (data.success) {
          messagesEl.innerHTML = "";
          addSystem("Switched to agent: " + name);
          await loadSession();
          await loadHistory();
        } else {
          addSystem("Switch failed: " + (data.error || "unknown"));
        }
      } catch (e) {
        addSystem("Switch failed: " + e.message);
      }
    });
  } catch (e) {
    console.warn("Failed to load agents:", e);
  }
}

function showError(msgEl, text) {
  msgEl.classList.remove("typing");
  msgEl.classList.remove("assistant");
  msgEl.classList.add("error");
  msgEl.textContent = "";
  const label = document.createElement("span");
  label.className = "role";
  label.textContent = "Error";
  msgEl.appendChild(label);
  msgEl.appendChild(document.createTextNode(text));
}

// Non-streaming fallback that talks to /api/chat.
async function sendMessage() {
  if (currentController) return;
  const text = inputEl.value.trim();
  if (!text) return;

  const requestId = Date.now() + "-" + Math.random().toString(36).slice(2);
  const controller = new AbortController();
  currentRequestId = requestId;
  currentController = controller;

  inputEl.value = "";
  autoResize();
  addMessage("user", text);

  sendBtn.textContent = "Cancel";
  sendBtn.disabled = false;
  const pending = addMessage("assistant", "");
  pending.classList.add("typing");
  pending.appendChild(document.createTextNode("Thinking"));

  try {
    const res = await fetch("/api/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ message: text, request_id: requestId }),
      signal: controller.signal
    });
    const data = await res.json();
    pending.classList.remove("typing");
    pending.textContent = "";

    if (data.success) {
      pending.appendChild(document.createTextNode(data.content || "(empty response)"));
    } else {
      showError(pending, data.error || "Request failed");
    }
  } catch (e) {
    if (e && e.name === "AbortError") {
      showError(pending, "Request cancelled.");
    } else {
      showError(pending, "Network error: " + (e && e.message ? e.message : e));
    }
  } finally {
    currentRequestId = null;
    currentController = null;
    sendBtn.textContent = "Send";
    sendBtn.disabled = false;
    inputEl.focus();
    messagesEl.scrollTop = messagesEl.scrollHeight;
  }
}

// Streaming send: consumes /api/chat/stream SSE and appends each token to
// the pending message as it arrives. Falls back to sendMessage() when the
// browser or the request cannot stream.
async function sendMessageStream() {
  if (currentController) return;
  const text = inputEl.value.trim();
  if (!text) return;

  if (!window.ReadableStream) {
    sendMessage();
    return;
  }

  const requestId = Date.now() + "-" + Math.random().toString(36).slice(2);
  const controller = new AbortController();
  currentRequestId = requestId;
  currentController = controller;
  sendBtn.textContent = "Cancel";
  sendBtn.disabled = false;

  let res;
  try {
    res = await fetch("/api/chat/stream", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ message: text, request_id: requestId }),
      signal: controller.signal
    });
  } catch (e) {
    if (e && e.name === "AbortError") {
      resetSendState();
      return;
    }
    resetSendState();
    sendMessage();
    return;
  }
  if (!res.ok || !res.body) {
    resetSendState();
    sendMessage();
    return;
  }

  inputEl.value = "";
  autoResize();
  addMessage("user", text);

  let textMsgEl = null;
  let textBody = null;
  let reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let done = false;

  try {
    while (!done) {
      const { value, done: streamDone } = await reader.read();
      if (streamDone) break;
      buffer += decoder.decode(value, { stream: true });
      let idx;
      while ((idx = buffer.indexOf("\n")) !== -1) {
        const line = buffer.slice(0, idx).replace(/\r$/, "");
        buffer = buffer.slice(idx + 1);
        if (!line.startsWith("data:")) continue;
        const payload = line.slice(5).trim();
        if (!payload) continue;
        if (payload === "[DONE]") {
          done = true;
          break;
        }
        let ev;
        try {
          ev = JSON.parse(payload);
        } catch (e) {
          continue;
        }
        if (ev && typeof ev.error === "string") {
          if (!textMsgEl) {
            textMsgEl = addMessage("assistant", "");
            textMsgEl.classList.add("typing");
            textBody = document.createTextNode("");
            textMsgEl.appendChild(textBody);
          }
          showError(textMsgEl, ev.error);
          done = true;
          break;
        }
        if (ev && Array.isArray(ev.tool_calls) && ev.tool_calls.length > 0) {
          addMessage("assistant", "", { tool_calls: ev.tool_calls });
          textMsgEl = null;
          textBody = null;
          continue;
        }
        if (ev && typeof ev.token === "string" && ev.token) {
          if (!textMsgEl) {
            textMsgEl = addMessage("assistant", "");
            textMsgEl.classList.add("typing");
            textBody = document.createTextNode("");
            textMsgEl.appendChild(textBody);
          }
          textBody.data += ev.token;
        }
      }
      messagesEl.scrollTop = messagesEl.scrollHeight;
    }
  } catch (e) {
    if (e && e.name === "AbortError") {
      if (!textMsgEl) {
        textMsgEl = addMessage("assistant", "");
        textMsgEl.classList.add("typing");
        textBody = document.createTextNode("");
        textMsgEl.appendChild(textBody);
      }
      showError(textMsgEl, "Request cancelled.");
    } else {
      if (!textMsgEl) {
        textMsgEl = addMessage("assistant", "");
        textMsgEl.classList.add("typing");
        textBody = document.createTextNode("");
        textMsgEl.appendChild(textBody);
      }
      showError(textMsgEl, "Network error: " + (e && e.message ? e.message : e));
    }
  } finally {
    try {
      await reader.cancel();
    } catch (e) {}
    if (textMsgEl) {
      if (textBody && textBody.data === "") {
        textBody.data = "(empty response)";
      }
      textMsgEl.classList.remove("typing");
    }
    resetSendState();
  }
}

function resetSendState() {
  currentRequestId = null;
  currentController = null;
  sendBtn.textContent = "Send";
  sendBtn.disabled = false;
  inputEl.focus();
  messagesEl.scrollTop = messagesEl.scrollHeight;
}

async function cancelRequest() {
  const controller = currentController;
  const requestId = currentRequestId;
  if (controller) controller.abort();
  if (requestId) {
    try {
      await fetch("/api/chat/cancel", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ request_id: requestId })
      });
    } catch (e) {
      console.warn("Cancel request failed:", e);
    }
  }
}

async function clearChat() {
  clearBtn.disabled = true;
  try {
    const res = await fetch("/api/clear", { method: "POST" });
    const data = await res.json();
    messagesEl.innerHTML = "";
    addSystem(data.success ? "Conversation cleared." : "Clear failed: " + (data.error || "unknown"));
    await loadSession();
    await loadHistory();
  } catch (e) {
    messagesEl.innerHTML = "";
    addSystem("Clear failed: " + e.message);
  } finally {
    clearBtn.disabled = false;
  }
}

function autoResize() {
  inputEl.style.height = "auto";
  inputEl.style.height = Math.min(inputEl.scrollHeight, 160) + "px";
}

sendBtn.addEventListener("click", () => {
  if (currentController) {
    cancelRequest();
  } else {
    sendMessageStream();
  }
});
clearBtn.addEventListener("click", clearChat);
inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    if (!currentController) sendMessageStream();
  }
});
inputEl.addEventListener("input", autoResize);

addSystem("Connected to pu serve. Send a message to start chatting.");
await loadSession();
await loadHistory();
await loadAgents();
