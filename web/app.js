"use strict";


const messagesEl = document.getElementById("messages");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("send");
let agentSelect = document.getElementById("agent-select");
const thinkingSelect = document.getElementById("thinking-select");
let agentChangeHandler = null;

// The level the server resolved: the session's own when it has one, the agent's otherwise.
// Only the server can tell those apart, so the control shows what it reports.
function renderThinkingControl(level, override) {
  const sessionLevel = override && override !== "default" ? override : null;
  thinkingSelect.value = sessionLevel || "auto";
  thinkingSelect.title =
    "Thinking: " + level +
    (sessionLevel ? " (set for this session)" : " (from the agent's configuration)");
}

async function setThinkingLevel(level) {
  try {
    const res = await fetch("/api/thinking", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ level }),
    });
    const data = await res.json();
    if (!data.success) {
      createSystemMessage("Thinking could not be set: " + (data.error || "unknown error"));
      return;
    }
    renderThinkingControl(data.thinking, data.thinking_override || null);
  } catch (e) {
    createSystemMessage("Thinking could not be set: " + e.message);
  }
}

// A change is the whole interaction: the level is a choice, not a step to repeat.
thinkingSelect.addEventListener("change", () => setThinkingLevel(thinkingSelect.value));

let ws = null;
let isStreaming = false;
let isAtBottom = true;
let stopRequested = false;
let reconnectAttempts = 0;
let reconnectTimer = null;
let connState = "connecting";

// How many nodes the store holds for the current chain. The next message sent
// becomes turn chainLength + 1, which is the number /api/rewind expects.
let chainLength = 0;

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
  return el;
}

function createMessage(role, blocks, turn, text) {
  const el = document.createElement("div");
  el.className = "msg " + role;

  const label = document.createElement("span");
  label.className = "role";
  label.textContent = role === "user" ? "You" : "Assistant";

  if (role === "user" && typeof turn === "number" && turn > 0) {
    const head = document.createElement("div");
    head.className = "msg-head";
    head.appendChild(label);

    const editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.className = "edit-btn";
    editBtn.textContent = "edit";
    editBtn.onclick = () => rewindToTurn(turn, text || "");
    head.appendChild(editBtn);

    el.appendChild(head);
  } else {
    el.appendChild(label);
  }

  const container = document.createElement("div");
  container.className = "blocks-container";
  el.appendChild(container);

  if (blocks) {
    renderBlocks(blocks, container);
  }

  messagesEl.appendChild(el);
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

// Tool output is the raw product of a command, so it is set as text:
// markup in a command's output is data rather than markup.
function labelledPre(wrapperClass, label, text, preClass) {
  const wrapper = document.createElement("div");
  wrapper.className = wrapperClass;
  const strong = document.createElement("strong");
  strong.textContent = label;
  const pre = document.createElement("pre");
  if (preClass) pre.className = preClass;
  pre.textContent = text;
  wrapper.append(strong, pre);
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
  } else if (block.status === "pending") {
    // Stored with no result: the turn ended before one was written, so what the
    // call did is unknown rather than successful.
    statusSpan.textContent = " ⚠ No result";
  } else if (block.error) {
    statusSpan.textContent = " ❌ Failed";
  } else {
    statusSpan.textContent = " ✅ Done";
  }
  header.appendChild(statusSpan);

  // A running command carries its own stop, on the block it would kill: the button sits
  // where the command does, so the thing stopped is the thing on screen.
  if (block.status === "running" && block.id) {
    const stop = document.createElement("button");
    stop.type = "button";
    stop.className = "stop-command";
    stop.textContent = "■ Stop";
    stop.title = "Stop this command";
    stop.onclick = (e) => {
      e.stopPropagation();
      askStopNote(wrapper, block.id);
    };
    header.appendChild(stop);
  }

  const icon = document.createElement("span");
  icon.className = "toggle-icon";
  icon.textContent = block.collapsed ? "▶" : "▼";
  header.appendChild(icon);

  const body = document.createElement("div");
  body.className = "block-body" + (block.collapsed ? " collapsed" : "");

  body.appendChild(
      labelledPre("tool-args", "Arguments", JSON.stringify(block.args ?? null, null, 2), ""));

  if (block.status === "done") {
    body.appendChild(labelledPre(
        "tool-result",
        block.error ? "Error" : "Output",
        block.error || block.output || "(no output)",
        block.error ? "tool-error" : ""));
  }

  wrapper.appendChild(header);
  wrapper.appendChild(body);
  return wrapper;
}

// Stopping a command is two steps in one place: the note is written against the command
// it explains, not in a modal that closes before the reason can be read. Enter stops,
// Esc leaves it running.
function askStopNote(wrapper, toolId) {
  if (wrapper.querySelector(".stop-note")) return;
  const body = wrapper.querySelector(".block-body");
  if (!body) return;
  wrapper.querySelector(".block-body")?.classList.remove("collapsed");
  const row = document.createElement("div");
  row.className = "stop-note";
  const input = document.createElement("input");
  input.type = "text";
  input.placeholder = "Why stop it? (Enter to stop, Esc to keep running)";
  const confirm = document.createElement("button");
  confirm.type = "button";
  confirm.textContent = "Stop";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.className = "stop-note-cancel";
  cancel.textContent = "Cancel";

  const commit = () => {
    const note = input.value.trim();
    row.remove();
    stopRunningCommand(toolId, note);
  };
  const dismiss = () => row.remove();

  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); commit(); }
    else if (e.key === "Escape") { e.preventDefault(); dismiss(); }
  });
  confirm.addEventListener("click", commit);
  cancel.addEventListener("click", dismiss);

  row.append(input, confirm, cancel);
  body.appendChild(row);
  input.focus();
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

let assistantRenderQueued = false;

// Coalesce a burst of tokens into one render per frame, and rebuild only the blocks that
// changed: re-parsing the whole answer per token re-renders text and code already shown.
function updateCurrentAssistantBlocks() {
  if (assistantRenderQueued) return;
  assistantRenderQueued = true;
  requestAnimationFrame(() => {
    assistantRenderQueued = false;
    renderAssistantBlocks();
  });
}

function renderAssistantBlocks() {
  if (!currentAssistantEl) return;
  const container = currentAssistantEl.querySelector(".blocks-container");
  if (!container) return;

  currentAssistantBlocks.forEach((block, i) => {
    if (!block.node) block.node = renderBlock(block);
    if (container.childNodes[i] !== block.node) {
      container.insertBefore(block.node, container.childNodes[i] || null);
    }
    if (block.type === BLOCK_TYPES.TEXT) {
      block.node.innerHTML = renderMarkdown(block.content);
    } else if (block.type === BLOCK_TYPES.THINKING) {
      const body = block.node.querySelector(".block-body");
      if (body) body.textContent = block.content;
    }
  });

  while (container.childNodes.length > currentAssistantBlocks.length) {
    container.removeChild(container.lastChild);
  }
  if (isAtBottom) messagesEl.scrollTop = messagesEl.scrollHeight;
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
  if (!block) return;
  block.output = payload.output || "";
  block.error = payload.error || "";
  block.status = "done";
  // The status and body changed, so this one block is rebuilt in place.
  if (block.node) {
    const rebuilt = renderBlock(block);
    block.node.replaceWith(rebuilt);
    block.node = rebuilt;
  }
  updateCurrentAssistantBlocks();
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

// Reasoning arrives on its own channel while the answer is still coming, and reads
// above the text in the same order the history renderer puts it in.
function handleThinking(payload) {
  const text = payload.text || "";
  if (!text) return;
  let block = currentAssistantBlocks.find((b) => b.type === BLOCK_TYPES.THINKING);
  if (!block) {
    block = { type: BLOCK_TYPES.THINKING, content: "", collapsed: false };
    currentAssistantBlocks.unshift(block);
  }
  block.content += text;
  updateCurrentAssistantBlocks();
}

// What the session talks through, read rather than chosen, so it lives in the panel
// where the other read-only facts do. The model is refreshed from every reply: a gateway
// may serve a different build, and the answer is the only place that says which.
let backendTypeShown = "";

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text || "—";
}

function setBackendLabel(backendType, model, host) {
  if (backendType) backendTypeShown = backendType;
  setText("kb-type", backendTypeShown);
  if (model) setText("kb-model", model);
  if (host) setText("kb-host", host);
}

function handleDone(payload) {
  finishAssistantMessage();
  setSendButtonState(false);
  refreshChainLength();
  if (stopRequested) {
    createSystemMessage("Stopped.");
    stopRequested = false;
  }

  if (payload && payload.model) {
    setBackendLabel(backendTypeShown, payload.model);
  }
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
  stopRequested = false;
  setSendButtonState(false);
  refreshChainLength();
}

// What the last turn ran against, from the counts the provider returned. The window
// itself is not shown: no gateway here reports one, and a guess would read like a fact.
function handleUsage(payload) {
  const el = document.getElementById("context-usage");
  const fill = document.getElementById("context-fill");
  if (!el || !payload) return;
  const prompt = Number(payload.prompt_tokens);
  const completion = Number(payload.completion_tokens);
  if (!Number.isFinite(prompt)) {
    el.textContent = "";
    if (fill) fill.style.width = "0";
    return;
  }
  el.textContent =
      `${prompt.toLocaleString()} tokens` +
      (Number.isFinite(completion) ? `  ·  +${completion.toLocaleString()}` : "");
  // No gateway here reports its window, so the bar can only be shown against one we
  // know; where none is known the number stands alone rather than a guess reading as fact.
  if (fill) {
    const window = contextWindowFor(backendTypeShown);
    if (window) {
      const ratio = Math.min(prompt / window, 1);
      fill.style.width = (ratio * 100).toFixed(1) + "%";
      fill.className = ratio > 0.85 ? "high" : (ratio > 0.6 ? "warn" : "");
    } else {
      fill.style.width = "0";
    }
  }
}

// Only what is certain: a window a backend is documented to have. An absent entry means
// the bar stays empty rather than implying a limit that was never probed.
const CONTEXT_WINDOWS = {};
function contextWindowFor(backendType) {
  return CONTEXT_WINDOWS[backendType] || null;
}

// A reply that arrived but is known to be incomplete. The answer stands as it is, and
// the remark is a strip above the composer rather than a line in the transcript, so it
// does not read as part of the conversation.
function showNotice(text) {
  const bar = document.getElementById("notice-bar");
  if (!bar) return;
  bar.textContent = text;
  const close = document.createElement("button");
  close.type = "button";
  close.textContent = "×";
  close.setAttribute("aria-label", "Dismiss");
  close.onclick = hideNotice;
  bar.appendChild(close);
  bar.hidden = false;
}
function hideNotice() {
  const bar = document.getElementById("notice-bar");
  if (bar) bar.hidden = true;
}
function handleNotice(payload) {
  const text = payload && payload.text ? payload.text : "";
  if (text) showNotice(text);
}

// One page per session: the server refuses a second one rather than ending the reply the
// first is watching — a state to report, not a connection to keep retrying.
const MAX_BUSY_RETRIES = 2;
let busyRefusals = 0;

function handleBusy(payload) {
  busyRefusals += 1;
  const reason = payload && payload.text ? payload.text : "This session is already open in another page.";
  showNotice(busyRefusals > MAX_BUSY_RETRIES
      ? reason + " Reload this page once it is free."
      : reason + " Trying again…");
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const url = `${protocol}//${window.location.host}/ws`;
  ws = new WebSocket(url);

  ws.onopen = () => {
    reconnectAttempts = 0;
    setConnectionState("online");
  };

  ws.onmessage = (event) => {
    let data;
    try { data = JSON.parse(event.data); } catch (_) { return; }

    // Anything but a refusal means this page is the client of the session, which is
    // what stops the knocking in onclose from being counted.
    if (data.type !== "busy") busyRefusals = 0;

    switch (data.type) {
      case "approval_ask":
        showApprovalBar(data.payload.command);
        break;
      case "tool_start":
        handleToolStart(data.payload);
        break;
      case "tool_end":
        handleToolEnd(data.payload);
        break;
      case "chunk":
        handleChunk(data.payload);
        break;
      case "thinking":
        handleThinking(data.payload);
        break;
      case "done":
        handleDone(data.payload);
        break;
      case "error":
        handleError(data.payload);
        break;
      case "notice":
        handleNotice(data.payload);
        break;
      case "usage":
        handleUsage(data.payload);
        break;
      case "busy":
        handleBusy(data.payload);
        break;
      default:
        break;
    }
  };

  // A dropped socket takes the reply with it: the turn is cancelled and keeps nothing, so
  // what was drawn here was all there was. The message stays, so the length is read back.
  ws.onclose = () => {
    const refused = busyRefusals > 0;
    if (isStreaming) {
      removeCurrentAssistantMessage();
      setSendButtonState(false);
      replyLostToDisconnect = true;
    }
    refreshChainLength();
    if (refused) {
      connState = "offline";
      if (busyRefusals <= MAX_BUSY_RETRIES) scheduleReconnect();
      return;
    }
    setConnectionState("offline");
    scheduleReconnect();
  };

  ws.onerror = () => {
    // onclose follows and schedules the reconnect; nothing to report here.
  };
}

// Set when a reply was being written as the socket went, so the reader is told the answer
// was stopped rather than left to wonder where it went.
let replyLostToDisconnect = false;

// A dropped socket reconnects on its own, backing off so a server that is down is not
// hammered. The state lives in the header dot; only a lost reply is worth a notice, since
// that is the one outcome the reader has to be told about.
function setConnectionState(state) {
  if (state === connState) return;
  const dot = document.getElementById("conn-dot");
  if (dot) {
    dot.className = "conn-dot " + state;
    dot.title = state === "online" ? "Connected"
      : (state === "offline" ? "Disconnected" : "Reconnecting…");
  }
  if (state === "offline" && connState === "online" && replyLostToDisconnect) {
    showNotice("Disconnected; the reply in progress was stopped and is not kept.");
  }
  if (state === "online" && connState === "offline") hideNotice();
  replyLostToDisconnect = false;
  connState = state;
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  const delay = Math.min(500 * 2 ** reconnectAttempts, 8000);
  reconnectAttempts += 1;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, delay);
}

function setSendButtonState(streaming) {
  isStreaming = streaming;
  // Cancelling the turn and stopping a command are different acts, so they are not both
  // called Stop: this one ends the reply, the command's own button ends the command.
  sendBtn.textContent = streaming ? "Cancel reply" : "Send";
  sendBtn.disabled = false;
  // The level applies to the next message, so it is locked while one is in flight
  // rather than left changeable into something that would not have applied.
  thinkingSelect.disabled = streaming;
}

// The store is the authority on how long the chain is, so the next turn number
// is read back from it after every run rather than counted here.
async function refreshChainLength() {
  try {
    const res = await fetch("/api/history");
    const history = await res.json();
    if (Array.isArray(history)) chainLength = history.length;
  } catch (_) {}
}

// Steps the session back to before `turn` and hands the text back to the composer, so
// sending it again replaces that turn.
async function rewindToTurn(turn, text) {
  if (isStreaming) {
    createSystemMessage("Cannot edit while a reply is streaming.");
    return;
  }
  try {
    const res = await fetch("/api/rewind", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ turn }),
    });
    const data = await res.json();
    if (!data.success) {
      createSystemMessage("Could not step back: " + (data.error || "unknown error"));
      return;
    }
    messagesEl.innerHTML = "";
    await loadHistory();
    inputEl.value = text;
    inputEl.focus();
    inputEl.style.height = "auto";
    createSystemMessage("Editing turn " + turn +
                        ". Sending replaces it; the turn it replaced is not kept.");
  } catch (e) {
    createSystemMessage("Could not step back: " + e.message);
  }
}

// One command, one stop: the id names which command, and the note is why. Serial turns
// make the id a formality today, but the frame already carries the note the result keeps.
function stopRunningCommand(toolId, note) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify({ type: "stop_command", payload: { text: note || "", id: toolId } }));
}

function sendMessage() {
  if (isStreaming) {
    // Cancelling only asks the server to stop, and the partial reply is dropped because
    // the store keeps none.
    if (ws && ws.readyState === WebSocket.OPEN) {
      stopRequested = true;
      ws.send(JSON.stringify({ type: "cancel" }));
      removeCurrentAssistantMessage();
      sendBtn.textContent = "Stopping…";
      sendBtn.disabled = true;
    } else {
      setSendButtonState(false);
    }
    return;
  }

  const text = inputEl.value.trim();
  if (!text) return;
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    createSystemMessage("Not connected to the server.");
    return;
  }
  stopRequested = false;
  inputEl.value = "";
  inputEl.style.height = "auto";

  // The store appends this message to the chain, so its turn is the next one;
  // the reply and any tool results follow it.
  createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: text }], chainLength + 1, text);
  startAssistantMessage();
  setSendButtonState(true);

  ws.send(JSON.stringify({ type: "run", payload: { text } }));
}

async function loadHistory() {
  try {
    const res = await fetch("/api/history");
    const history = await res.json();
    if (!Array.isArray(history)) return;

    // Grouped rather than walked one message at a time: a tool-using turn is several
    // stored messages, and drawing each alone would show another conversation.
    for (const turn of groupHistory(history)) {
      if (turn.role === "system") {
        createSystemMessage(turn.text);
      } else if (turn.role === "user") {
        createMessage("user", [{ type: BLOCK_TYPES.TEXT, content: turn.text }], turn.turn,
                      turn.text);
      } else {
        createMessage("assistant", turn.blocks);
      }
    }
    chainLength = history.length;
  } catch (_) {}
}

async function loadSession() {
  try {
    const res = await fetch("/api/session");
    const data = await res.json();
    if (data.success) {
      setBackendLabel(data.backend_type, data.backend_model, data.backend_host);
      // Offered only where a level lands: a backend that ignores one shows nothing
      // rather than a control that would do nothing.
      thinkingSelect.hidden = !data.supports_thinking_level;
      if (data.supports_thinking_level) {
        renderThinkingControl(data.thinking || "default", data.thinking_override || null);
      }
      if (data.agent_name) {
        agentSelect.value = data.agent_name;
      }
      renderToolList(data.tools);
    }
  } catch (_) {}
}

// The tools the active agent holds, as the session reports them. A name under the
// `mcp.` prefix came from a server rather than the built-in set.
function renderToolList(tools) {
  const list = document.getElementById("panel-tools");
  if (!list) return;
  list.innerHTML = "";
  if (!Array.isArray(tools) || tools.length === 0) {
    const li = document.createElement("li");
    li.textContent = "—";
    li.style.color = "var(--ink-light)";
    list.appendChild(li);
    return;
  }
  for (const name of tools) {
    const li = document.createElement("li");
    li.textContent = name;
    if (name.startsWith("mcp.")) {
      const src = document.createElement("span");
      src.className = "tool-src";
      src.textContent = "mcp";
      li.appendChild(src);
    }
    list.appendChild(li);
  }
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
// While an input method is composing, Enter confirms a candidate rather than sending;
// the flag keeps a Chinese candidate from firing the message the moment it is picked.
let composing = false;
inputEl.addEventListener("compositionstart", () => { composing = true; });
inputEl.addEventListener("compositionend", () => { composing = false; });
inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !composing) {
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

const BLOCK_TYPES = {
  THINKING: "thinking",
  TOOL_CALL: "tool_call",
  TEXT: "text",
};

// Arguments travel as an object or an encoded string; a string that is not JSON is kept as
// it came, because one unreadable call must not hide the rest of the turn.
function parseToolArguments(raw) {
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw);
    } catch (_) {
      return raw;
    }
  }
  return raw === undefined ? null : raw;
}

// What the store recorded: a result answered the call, or nothing has. "running"
// is the stream's word for a call in flight and is never stored.
function callStatus(statuses, index) {
  return statuses[index] === "pending" ? "pending" : "done";
}

// Rebuilds the transcript from the stored conversation: a node per provider message is
// grouped into a bubble per turn, the way the streaming path builds them.
function groupHistory(history) {
  const turns = [];
  let reply = null;

  const closeReply = () => {
    if (reply) turns.push(reply);
    reply = null;
  };
  const currentReply = () => {
    if (!reply) reply = { role: "assistant", blocks: [] };
    return reply;
  };

  for (const msg of history) {
    const role = msg.role;
    const content = msg.content || "";

    if (role === "system") {
      closeReply();
      turns.push({ role: "system", text: content });
      continue;
    }

    if (role === "user") {
      closeReply();
      turns.push({ role: "user", text: content, turn: msg.id });
      continue;
    }

    if (role === "tool") {
      const bubble = currentReply();
      const block = bubble.blocks.find(
        (b) => b.type === BLOCK_TYPES.TOOL_CALL && b.id === msg.tool_call_id
      );
      if (!block) continue;
      // `output` and `error` are the result as the running turn parsed it; the raw
      // content is what is left when the server could not parse it either.
      block.output = typeof msg.output === "string" ? msg.output : content;
      block.error = msg.error || "";
      block.status = "done";
      continue;
    }

    if (role !== "assistant") continue;

    const bubble = currentReply();

    if (Array.isArray(msg.reasoning) && msg.reasoning.length > 0) {
      // One thinking block per reply, above the rest, where the streaming path puts it and
      // appends every iteration's reasoning. Blocks are concatenated back into one blob.
      const text = msg.reasoning.map((block) => block.text || "").join("");
      let thinking = bubble.blocks.find((b) => b.type === BLOCK_TYPES.THINKING);
      if (!thinking) {
        thinking = { type: BLOCK_TYPES.THINKING, content: "", collapsed: true };
        bubble.blocks.unshift(thinking);
      }
      thinking.content += text;
    }

    const calls = Array.isArray(msg.tool_calls) ? msg.tool_calls : [];
    const statuses = Array.isArray(msg.tool_call_status) ? msg.tool_call_status : [];
    calls.forEach((call, index) => {
      const fn = call.function || {};
      bubble.blocks.push({
        type: BLOCK_TYPES.TOOL_CALL,
        id: call.id || "unknown",
        name: fn.name || "unknown",
        args: parseToolArguments(fn.arguments),
        output: "",
        error: "",
        status: callStatus(statuses, index),
        collapsed: true,
      });
    });

    if (content) bubble.blocks.push({ type: BLOCK_TYPES.TEXT, content });
  }

  closeReply();
  return turns;
}
