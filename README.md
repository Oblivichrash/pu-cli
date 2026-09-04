# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **single-session auto‑persistence** and **dynamic backend switching**.

---

## Quick Start

### Build

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

- **Real-time streaming chat** — replies appear token by token (typewriter effect) via `POST /api/chat/stream` (SSE).
- **Request cancellation** — the Send button turns into Cancel while a request is in flight (`POST /api/chat/cancel`).
- **Agent switching** — pick an agent from the dropdown (`POST /api/agent/switch`).
- **History loading** — previous messages are restored from the persisted session (`GET /api/history`).

The front-end lives in `web/` and talks to the runtime through a small JSON API;
when streaming is unavailable (old browser or server) it automatically falls back
to the non-streaming `POST /api/chat` endpoint.

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

Note: The limits configuration section (max_sessions, max_history_messages, max_branches) is no longer effective and should not be used.

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
