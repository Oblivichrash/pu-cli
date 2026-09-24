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
  vcpkg install boost-system boost-program-options boost-json boost-asio boost-beast spdlog openssl catch2
  ```

### Build

A C++23 compiler and CMake >= 3.16 are required. Point CMake at your vcpkg
toolchain on Windows (`-DCMAKE_TOOLCHAIN_FILE=...`).

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Configure

Create `agents.json` inside a `.pu/` directory — either `./.pu/agents.json` (project-level) or `~/.pu/agents.json` (user-level, resolved from `HOME`):

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
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

The example above is a working minimum; every field is listed under
[Configuration](#agentsjson).

### Usage

| Command | Description |
| :------ | :---------- |
| `pu ask <prompt>` | Send one prompt, print the reply, and exit |
| `pu chat` | Start an interactive session |
| `pu serve [--host H] [--port P]` | Start the Web UI (see [Web Server](#web-server-pu-serve)) |

Global options: `-h`/`--help` and `--version`. `ask` and `chat` also accept
`--agent <name>` to start with a non-default agent; `ask` accepts
`--prompt <text>` as an alternative to the positional prompt.

```bash
./build/pu ask "summarize README.md"
./build/pu chat --agent local-assistant
> /backend deepseek-pro    # switch to predefined agent
```

Your conversation is automatically saved to `<data-dir>/session.json` (by
default `./.pu/session.json`) after every interaction, and restored when you
restart. Each directory has its own independent session.

### Web Server (`pu serve`)

`pu serve` starts a local web UI on top of the same single-session runtime. Open
`http://127.0.0.1:8080` in a browser and chat with the active agent:

```bash
./build/pu serve                       # listen on 127.0.0.1:8080
./build/pu serve --host 0.0.0.0 --port 9000
```

> **Warning** — the server has no authentication and no origin checks, so
> anyone who can reach the port can read the session history, switch
> workspaces, and run the active agent's tools. Keep the default loopback
> bind unless the port is protected by other means.

Each message you sent carries an **edit** link. It steps the session back to just
before that turn and puts the text back in the composer; sending it again
replaces that turn, and the turn it replaced is dropped when the replacement
lands, so what the session file ends up holding is what sending the new text from
the start would have left.

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
{"type":"thinking","payload":{"text":"reasoning part"}}
{"type":"tool_start","payload":{"id":"call_1","name":"execute_bash","args":{"command":"ls"}}}
{"type":"tool_end","payload":{"id":"call_1","output":"...","error":""}}
{"type":"notice","payload":{"text":"the reply stopped at the token limit..."}}
{"type":"done","payload":{"model":"gpt-4o-mini-2024-07-18"}}
{"type":"error","payload":{"text":"error description"}}
```

The server streams back chunks as they are generated; the front-end renders them incrementally. `tool_start` is emitted just before a tool runs and `tool_end` when it returns; both carry the tool call `id` so the UI can pair a result with the call it belongs to. Cancellation interrupts the in-flight LLM request: the server only sets the cancel token, and the client closes the WebSocket itself after sending `{"type":"cancel"}`.

`thinking` carries the model's reasoning, which is a channel of its own: the front-end renders it beside the answer rather than in it, and a backend that reports no reasoning simply sends none.

`notice` is a remark about a reply that arrived but is known to be incomplete — the provider stopped it at the token limit or its content filter. It is sent before the turn is closed and is not an error: the reply itself was stored and stands as it is.

`done` carries the model the response named, when it named one: a gateway may serve a different build than the one that was configured, and the header then shows who replied.

#### REST API Endpoints

| Method | Path | Description |
| :----- | :--- | :---------- |
| `GET` | `/api/session` | Current session info (agent, backend type/model) |
| `GET` | `/api/history` | Full conversation history (user, assistant and tool messages) |
| `GET` | `/api/agents` | List all available agents with descriptions |
| `POST` | `/api/agent/switch` | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/workspaces` | List all workspaces (directories containing `.pu/agents.json`) |
| `POST` | `/api/workspace/switch` | Switch workspace (`{"path":"..."}`) |
| `POST` | `/api/clear` | Clear the conversation history |
| `POST` | `/api/rewind` | Step back to before a turn (`{"turn":n}`); the next message replaces it |
| `POST` | `/api/thinking` | Set this session's thinking level (`{"level":"auto\|none\|low\|medium\|high\|default"}`) |

All endpoints return JSON. The chat functionality is exclusively provided by the WebSocket; the REST API is for control and status.

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/backend <agent>` | Switch to predefined agent (rebuilds tool set) |
| `/backend <type> <model> [host] [api_key]` | Give this session a backend of its own, outranking `agents.json` |
| `/agents` | List available agents |
| `/clear` | Clear conversation history |
| `/rewind <turn>` | Step back to before a turn; the next message replaces it |
| `/thinking [level]` | Show or set this session's thinking level (`auto` follows the agent's configuration) |
| `/exit`, `/quit` | Exit |

These are chat commands. The Web server is a CLI subcommand (`pu serve`),
not a chat command.

---

## Tools & Agent Binding

Tool set is bound to the active agent – switching agents with `/backend <agent>` automatically rebuilds the registry (stops previous MCP servers, starts new ones).

Built-in tools:

| Tool | Description |
|------|-------------|
| `execute_bash` | Run a shell command in the sandbox, subject to the security policy |
| `write_file` | Write a file in the sandbox |

- MCP tools are exposed with a `mcp.<server>.<tool>` prefix.

### Tool Output Format

All tools return structured JSON with the following fields:

```json
{
  "success": true/false,
  "stdout": "...",
  "stderr": "...",
  "error": "...",
  "exit_code": 0
}
```

This allows the executor to distinguish success from failure and provide clear feedback to the model. The transcript keeps the result verbatim, so the model sees the same JSON the tool produced and nothing is lost in extraction.

---

## Configuration

### `agents.json`

The configuration file must be located in a `.pu/` directory. Search order is
`./.pu/agents.json` (relative to the working directory) then
`~/.pu/agents.json` (resolved from `HOME`, which is not set by default on
Windows).

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
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

**Agent fields:**

| Field | Description |
|-------|-------------|
| `name` | Identifier used by `default_agent`, `/backend <agent>`, and `--agent` |
| `description` | Optional text shown when the agent connects |
| `backend` | Required object; see the backend fields below |
| `security` | Optional security policy; see [Security](#security) |
| `mcp_servers` | Optional MCP servers; see [MCP Servers](#mcp-servers) |

**Backend fields:**

| Field | Default | Description |
|-------|---------|-------------|
| `type` | `ollama` | `ollama` or `openai`; any other value is rejected |
| `host` | — | Required. Base URL |
| `model` | — | Required |
| `api_key` | unset | Sent as `Authorization: Bearer` when set |
| `temperature` | `0.7` | |
| `max_tokens` | `2048` | Sent by the OpenAI-compatible path only |
| `thinking` | `default` | `none`, `low`, `medium`, `high`, or `default` (send nothing and let the backend decide); OpenAI-compatible path only |
| `system_prompt` | unset | Agent-specific system prompt, merged with the injected context |

`host`, `model`, `api_key`, `system_prompt`, and the MCP `url`/`headers` values
support `${ENV_VAR}` expansion; an unset variable expands to an empty string and
logs a warning.

### Security

- `forbidden_patterns` – commands containing these substrings are blocked.
- `sandbox_root` – all file operations are relative to this directory.
- `max_command_length` – reject a shell command longer than this many bytes (`0` disables the check).
- It is strongly recommended to include `"cd"` in `forbidden_patterns` to prevent the model from changing the working directory, which can cause confusion.

### MCP Servers

MCP servers can be launched as local subprocesses (stdio) or reached over a
remote HTTP endpoint (streamable HTTP). The transport is selected automatically:
if `url` is present the client uses HTTP, otherwise it spawns the `command`.

**stdio (local subprocess):**

```json
"mcp_servers": [
  {
    "name": "filesystem",
    "command": "npx",
    "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
  }
]
```

**HTTP (remote):**

```json
"mcp_servers": [
  {
    "name": "remote-fs",
    "url": "https://mcp.example.com/mcp",
    "headers": {
      "Authorization": "Bearer ${MCP_API_TOKEN}"
    }
  }
]
```

`mcp_servers` sits inside an agent, beside `backend`; see
[Configuration](#agentsjson) for the surrounding shape.

| Field | Description |
|-------|-------------|
| `name` | Display name for the MCP server |
| `command` | Executable to launch (stdio transport; ignored when `url` is set) |
| `args` | Arguments passed to the executable (stdio transport) |
| `url` | Remote streamable-HTTP MCP endpoint. When present, HTTP transport is used instead of stdio |
| `headers` | Optional HTTP headers sent with every request, e.g. `Authorization` (values support `${ENV_VAR}` expansion) |

### Thinking Mode

`thinking` applies to the OpenAI-compatible backend only, and it is a level rather
than a switch: `none` sends `thinking.type = "disabled"` so a model that would
otherwise reason answers directly, `low`/`medium`/`high` are sent as
`reasoning_effort`, and `default` sends neither, which leaves the choice to the
backend. A file written before the level existed keeps its meaning:
`enable_thinking: false` reads as `none`, and `enable_thinking: true` as `default`.

The level can also be set for one session without editing this file: `/thinking
<level>` in the CLI, or the control beside the composer in the Web UI, and either
is remembered with the session. A backend that does not carry a level — Ollama,
where the model decides for itself — says so, and no control is offered.

```json
"backend": {
  "type": "openai",
  "host": "https://api.deepseek.com/v1",
  "model": "deepseek-reasoner",
  "api_key": "${DEEPSEEK_API_KEY}",
  "thinking": "high"
}
```

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_HOME` | Overrides the data directory used for logs (default `./.pu/`). Only logging is affected: `session.json` comes from the workspace's `.pu/`, and `agents.json` from there or from `~/.pu/` |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve`. Defaults to the first existing of `./web`, `../share/pu/web`, `/usr/share/pu/web`, `/usr/local/share/pu/web` |
| `PU_SERVE_HOST` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |

### Logging

- The console only shows `error` and `critical` messages; `info`, `warn`, `debug`, and `trace` are never printed to the console.
- Use `PU_LOG_LEVEL` to control the file log verbosity (default `info`).
- Log files go to `<data-dir>/logs/pu.log`, rotate at 5MB, and keep 3 files.

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
