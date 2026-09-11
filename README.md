# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **single-session auto‑persistence** and **dynamic backend switching**.

---

## Quick Start

### Build Dependencies

pu-cli uses **Boost.JSON** (Boost >= 1.75) for all JSON parsing and
serialization. The runtime dependencies are **Boost** (Beast, Asio, JSON,
ProgramOptions), **spdlog**, and **OpenSSL** (Catch2 is only needed for unit tests).

Install the dependencies first:

- **Linux (Debian/Ubuntu)** — either the full Boost package or individual libraries:

  ```bash
  sudo apt-get install -y libboost-system-dev libboost-program-options-dev \
      libboost-json-dev libspdlog-dev libssl-dev catch2
  # or: sudo apt-get install -y libboost-all-dev
  ```

- **macOS**:

  ```bash
  brew install boost spdlog openssl catch2
  ```

- **Windows (vcpkg)**:

  ```bash
  vcpkg install boost-system boost-program-options boost-json spdlog openssl catch2
  ```

### Build

A C++23 compiler and CMake >= 3.16 are required. Point CMake at your vcpkg
toolchain on Windows (`-DCMAKE_TOOLCHAIN_FILE=...`).

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Configure

Create `agents.json` inside a `.pu/` directory — either `./.pu/agents.json` (project-level) or `~/.pu/agents.json` (user-level):

```json
{
  "default_agent": "local-assistant",
  "agents": [
    {
      "name": "local-assistant",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:2b"
      },
      "tools": ["execute_bash", "write_file"],
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

### Usage

```bash
./build/pu chat
> /backend deepseek-pro    # switch to predefined agent
```

Your conversation is automatically saved to `./.pu/session.json` after every interaction, and restored when you restart. Each directory has its own independent session.

### Web Server (`pu serve`)

`pu serve` starts a local web UI on top of the same single-session runtime. Open
`http://127.0.0.1:8080` in a browser and chat with the active agent:

```bash
./build/pu serve                       # listen on 127.0.0.1:8080
./build/pu serve --host 0.0.0.0 --port 9000
```

The Web UI supports:

- **Real-time streaming chat** — replies appear token by token (typewriter effect) via **WebSocket** (`ws://` endpoint `/ws`).
- **Request cancellation** — the Send button turns into Cancel while a request is in flight; click it to abort the current generation.
- **Agent switching** — pick an agent from the dropdown (`POST /api/agent/switch`).
- **History loading** — previous messages are restored from the persisted session (`GET /api/history`).
- **Workspace switching** — switch between different project directories (each with its own `.pu/` configuration and session).

The front-end lives in `web/` and talks to the runtime through a small JSON API over WebSocket for chat, plus REST endpoints for state queries and actions.

#### WebSocket Protocol

**Endpoint**: `ws://<host>:<port>/ws`

**Client → Server**:

```json
{"type":"run","payload":{"text":"user message"}}
{"type":"cancel"}
```

**Server → Client**:

```json
{"type":"chunk","payload":{"text":"token part"}}
{"type":"tool_start","payload":{"id":"call_1","name":"execute_bash","args":{"command":"ls"}}}
{"type":"tool_end","payload":{"id":"call_1","output":"...","error":""}}
{"type":"done"}
{"type":"error","payload":{"text":"error description"}}
```

The server streams back chunks as they are generated; the front-end renders them incrementally. `tool_start` is emitted just before a tool runs and `tool_end` when it returns; both carry the tool call `id` so the UI can pair a result with the call it belongs to. Cancellation immediately interrupts the LLM request and closes the WebSocket.

#### REST API Endpoints

| Method | Path | Description |
| :----- | :--- | :---------- |
| `GET` | `/api/session` | Current session info (agent, backend type/model) |
| `GET` | `/api/history` | Full conversation history (including system and tool messages) |
| `GET` | `/api/agents` | List all available agents with descriptions |
| `POST` | `/api/agent/switch` | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/workspaces` | List all workspaces (directories containing `.pu/agents.json`) |
| `POST` | `/api/workspace/switch` | Switch workspace (`{"path":"..."}`) |
| `POST` | `/api/clear` | Clear the conversation history |

All endpoints return JSON. The chat functionality is exclusively provided by the WebSocket; the REST API is for control and status.

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/backend <agent>` | Switch to predefined agent (rebuilds tool set) |
| `/backend <type> <model>` | Manual backend switch |
| `/agents` | List available agents |
| `/clear` | Clear conversation history |
| `/serve` | Start the Web chat server (see `pu serve` in Usage) |
| `/exit`, `/quit` | Exit |

---

## Tools & Agent Binding

Tool set is bound to the active agent – switching agents with `/backend <agent>` automatically rebuilds the registry (stops previous MCP servers, starts new ones).

- `tools` in `agents.json` filters built‑in tools.
- MCP tools are exposed with a `mcp.<server>.<tool>` prefix.

### Tool Output Format

All tools now return structured JSON with the following fields:

```json
{
  "success": true/false,
  "stdout": "...",
  "stderr": "...",
  "error": "...",
  "exit_code": 0
}
```

This allows the executor to distinguish success from failure and provide clear feedback to the model. The transcript stores the extracted `stdout` or `error` content; the full JSON is not persisted.

---

## Configuration

### `agents.json`

The configuration file must be located in a `.pu/` directory. Search order is `./.pu/agents.json` then `~/.pu/agents.json`.

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:4b",
        "temperature": 0.7,
        "max_tokens": 4096
      },
      "tools": ["execute_bash", "write_file"],
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

### Security

- `forbidden_patterns` – commands containing these substrings are blocked.
- `sandbox_root` – all file operations are relative to this directory.
- It is strongly recommended to include `"cd"` in `forbidden_patterns` to prevent the model from changing the working directory, which can cause confusion.

### MCP Servers

MCP servers can be launched as local subprocesses (stdio) or reached over a
remote HTTP endpoint (streamable HTTP). The transport is selected automatically:
if `url` is present the client uses HTTP, otherwise it spawns the `command`.

**stdio (local subprocess):**

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "backend": { "type": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:4b" },
      "tools": ["execute_bash", "write_file"],
      "mcp_servers": [
        {
          "name": "filesystem",
          "command": "npx",
          "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
        }
      ]
    }
  ]
}
```

**HTTP (remote):**

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "backend": { "type": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:4b" },
      "tools": ["execute_bash", "write_file"],
      "mcp_servers": [
        {
          "name": "remote-fs",
          "url": "https://mcp.example.com/mcp",
          "headers": {
            "Authorization": "Bearer ${MCP_API_TOKEN}"
          }
        }
      ]
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `name` | Display name for the MCP server |
| `command` | Executable to launch (stdio transport; ignored when `url` is set) |
| `args` | Arguments passed to the executable (stdio transport) |
| `url` | Remote streamable-HTTP MCP endpoint. When present, HTTP transport is used instead of stdio |
| `headers` | Optional HTTP headers sent with every request, e.g. `Authorization` (values support `${ENV_VAR}` expansion) |

### Thinking Mode & History Compaction

```json
{
  "default_agent": "deepseek",
  "agents": [
    {
      "name": "deepseek",
      "backend": {
        "type": "openai",
        "host": "https://api.deepseek.com/v1",
        "model": "deepseek-reasoner",
        "api_key": "${DEEPSEEK_API_KEY}",
        "enable_thinking": true,
        "temperature": 0.1
      },
      "history_compaction": {
        "enabled": false,
        "keep_head": 15,
        "keep_tail": 60
      }
    }
  ]
}
```

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_HOME` | Overrides the data directory (default `./.pu/`) |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve` (default: auto-detected `web/` next to the binary or in the working directory) |
| `PU_SERVE_HOST` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |

### Logging

- The console only shows `error` and `critical` messages; `info`, `warn`, `debug`, and `trace` are never printed to the console.
- Use `PU_LOG_LEVEL` to control the file log verbosity (default `info`).
- Log files are stored in `<data-dir>/logs/pu.log` (rotated, max 5MB per file, 3 files kept).
- The data directory is `PU_HOME` if set, otherwise `./.pu/`.

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
