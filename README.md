# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **single-session auto‑persistence** and **dynamic backend switching**.

Internals and layering live in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Quick Start

### Build Dependencies

Runtime: **Boost >= 1.75** (Beast, Asio, JSON, ProgramOptions), **spdlog**, and
**OpenSSL**. Catch2 is needed only for tests. The exact package lists live in
[.github/workflows/ci.yml](.github/workflows/ci.yml); the common installs are:

```bash
sudo apt-get install -y libboost-all-dev libspdlog-dev libssl-dev catch2   # Debian/Ubuntu
brew install boost spdlog openssl catch2                                   # macOS
vcpkg install boost-asio boost-beast boost-json boost-program-options spdlog openssl catch2  # Windows
```

### Build

A C++23 compiler and CMake >= 3.16 are required. Point CMake at your vcpkg
toolchain on Windows (`-DCMAKE_TOOLCHAIN_FILE=...`).

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
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

Every field is listed under [Configuration](#agentsjson).

### Usage

| Command | Description |
| :------ | :---------- |
| `pu ask <prompt>` | Send one prompt, print the reply, and exit |
| `pu chat` | Start an interactive session |
| `pu serve [--host H] [--port P]` | Start the Web UI (see [Web Server](#web-server-pu-serve)) |

Global options: `-h`/`--help` and `--version`. `ask` and `chat` also accept
`--agent <name>`; `ask` accepts `--prompt <text>` in place of the positional
prompt.

```bash
./build/pu ask "summarize README.md"
./build/pu chat --agent local-assistant
> /backend deepseek-pro    # switch to predefined agent
```

Your conversation is saved to `<workspace>/.pu/session.json` after every
interaction and restored on restart, so each directory keeps its own session.

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/backend <agent>` | Switch to a predefined agent (rebuilds the tool set) |
| `/backend <type> <model> [host] [api_key]` | Override the backend for this session, outranking `agents.json` |
| `/agents` | List available agents |
| `/clear` | Clear conversation history |
| `/rewind <turn>` | Step back to before a turn; the next message replaces it |
| `/thinking [level]` | Show or set this session's thinking level (`auto` follows the agent's configuration) |
| `/exit`, `/quit` | Exit |

`pu serve` is a CLI subcommand, not a chat command.

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

The executor reads `success` and `stdout`/`error` from it; the transcript keeps the result verbatim, so the model sees what the tool produced.

---

## Web Server (`pu serve`)

`pu serve` starts a local web UI on top of the same single-session runtime. Open
`http://127.0.0.1:8080` in a browser and chat with the active agent:

```bash
./build/pu serve                       # listen on 127.0.0.1:8080
./build/pu serve --host 0.0.0.0 --port 9000
```

A server serves the directory it was started in and stays there: nothing moves it to
another one. Each directory is a session with its own configuration, conversation and
tools, so serving several of them at once means one server per directory — which is
also what keeps them from sharing a conversation or a turn. Since that needs a port per
directory, the workspace can name its own (`--host`/`--port` first, then
`PU_SERVE_HOST`/`PU_SERVE_PORT`, then the file):

```json
"serve": { "host": "127.0.0.1", "port": 8087 }
```

> **Warning** — the server has no authentication and no origin checks, so
> anyone who can reach the port can read the session history and run the active
> agent's tools. Keep the default loopback bind unless the port is protected by
> other means.

Each message carries an **edit** link that steps back to before that turn; sending
the text again replaces it. See `/rewind` below.

The front-end lives in `web/`. History is folded into one bubble per turn, with
each tool result keeping the output the stream showed.

### Web API

Chat is a WebSocket at `ws://<host>:<port>/ws`. Everything else is REST, for control
and status.

**Client → server**

```json
{"type":"run","payload":{"text":"user message"}}
{"type":"cancel"}
```

**Server → client**

```json
{"type":"chunk","payload":{"text":"token part"}}
{"type":"thinking","payload":{"text":"reasoning part"}}
{"type":"tool_start","payload":{"id":"call_1","name":"execute_bash","args":{"command":"ls"}}}
{"type":"tool_end","payload":{"id":"call_1","output":"...","error":""}}
{"type":"notice","payload":{"text":"the reply stopped at the token limit..."}}
{"type":"busy","payload":{"text":"This session is already open in another page."}}
{"type":"done","payload":{"model":"gpt-4o-mini-2024-07-18"}}
{"type":"error","payload":{"text":"error description"}}
```

`tool_start`/`tool_end` carry the tool call `id` so the UI pairs a result with
its call. `cancel` sets the cancel token and leaves the connection open; the turn
then ends with `done`. `thinking` is a channel of its own, rendered beside the
answer; a backend that reports no reasoning sends none. `notice` is a reply that
is known to be incomplete (token limit or content filter) and is not an error.
`done` names the model that answered, which a gateway may have chosen.

A turn belongs to the client watching it: `cancel`, closing the page, or losing the
connection all end it, and what it had written is not kept, so the session is left
holding the question and no answer. Reconnecting therefore shows the conversation
without the reply that was in progress.

A session has one page. While a page is attached, a second connection is answered with
`busy` and closed rather than taking over, since taking over would mean ending the
reply the page already there is reading.

**REST endpoints**

| Method | Path | Description |
| :----- | :--- | :---------- |
| `GET` | `/api/session` | Current session info (agent, backend type/model) |
| `GET` | `/api/history` | Full conversation history (user, assistant and tool messages); every tool call carries `tool_call_status` and every result the parsed `output`/`error` the stream showed |
| `GET` | `/api/agents` | List all available agents with descriptions |
| `POST` | `/api/agent/switch` | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/workspaces` | List all workspaces (directories containing `.pu/agents.json`) |
| `POST` | `/api/clear` | Clear the conversation history |
| `POST` | `/api/rewind` | Step back to before a turn (`{"turn":n}`); the next message replaces it |
| `POST` | `/api/thinking` | Set this session's thinking level (`{"level":"auto\|none\|low\|medium\|high\|default"}`) |

A request that fails is answered with the status that names whose it is: `400` for one the
caller can fix — a body that is not JSON, a value that is missing or unknown, a state that
cannot serve it, such as a step back while a tool call is pending — `404` where a name does
not exist, and `500` where the operation failed. A refusal carries `success: false` and the
reason in `error`.

---

## Configuration

### `agents.json`

The file lives in a `.pu/` directory: `./.pu/agents.json` (project) or
`~/.pu/agents.json` (user, resolved from `HOME`, which Windows does not set by
default). See [Quick Start](#configure) for a full example.

**Top-level fields**

| Field | Description |
|-------|-------------|
| `default_agent` | Required; must name one of the entries in `agents` |
| `agents` | Required; the entries below |
| `serve` | Optional; `{"host": ..., "port": ...}` for `pu serve`, used for whichever of the two neither the command line nor the environment names |

**Agent fields**

| Field | Description |
|-------|-------------|
| `name` | Identifier used by `default_agent`, `/backend <agent>`, and `--agent` |
| `description` | Optional text shown when the agent connects |
| `backend` | Required object; see below |
| `security` | Optional; see below |
| `mcp_servers` | Optional; see below |

**Backend fields**

| Field | Default | Description |
|-------|---------|-------------|
| `type` | `ollama` | `ollama`, `openai`, or `codebuddy`; any other value is rejected |
| `host` | per type | Base URL; omitted, it falls back to `http://localhost:11434` (ollama), `https://api.openai.com/v1` (openai), or the CodeBuddy gateway |
| `model` | — | Required |
| `api_key` | unset | Sent as `Authorization: Bearer` when set |
| `temperature` | `0.7` | |
| `max_tokens` | `2048` | Sent by the OpenAI-compatible path only |
| `thinking` | `default` | `none`, `low`, `medium`, `high`, or `default`; the level is sent only on the OpenAI-compatible path — `none` as `thinking.type = "disabled"`, `low`/`medium`/`high` as `reasoning_effort`, `default` as neither. `enable_thinking: false|true` from an older file reads as `none|default`. Set it per session with `/thinking`, the Web header, or `POST /api/thinking` |
| `system_prompt` | unset | Merged with the injected context |

`codebuddy` is the `openai` protocol against the CodeBuddy cloud gateway; the
gateway's contract is in
[docs/design/codebuddy-cloud-api.md](docs/design/codebuddy-cloud-api.md). Its
supported models are the account's, so a request naming one the account may not
use is answered by the gateway itself.

```json
"backend": {
  "type": "codebuddy",
  "host": "https://copilot.tencent.com/v2",
  "model": "deepseek-v4-flash",
  "api_key": "${CODEBUDDY_API_KEY}"
}
```

`host`, `model`, `api_key`, `system_prompt`, and the MCP `url`/`headers` values
support `${ENV_VAR}` expansion; an unset variable expands to an empty string and
logs a warning.

**`security`**

| Field | Description |
|-------|-------------|
| `sandbox_root` | All file operations are relative to this directory |
| `forbidden_patterns` | Commands containing these substrings are blocked; include `"cd"` so the model cannot change the working directory |
| `max_command_length` | Reject a shell command longer than this many bytes (`0` disables the check) |

**`mcp_servers`**

Each entry is a stdio subprocess or an HTTP endpoint, selected by whether `url`
is set. `mcp_servers` sits inside an agent, beside `backend`.

```json
{"name": "filesystem", "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]}
{"name": "remote-fs", "url": "https://mcp.example.com/mcp", "headers": {"Authorization": "Bearer ${MCP_API_TOKEN}"}}
```

| Field | Description |
|-------|-------------|
| `name` | Display name for the MCP server |
| `command` / `args` | Executable and arguments to launch (stdio only) |
| `url` / `headers` | Remote streamable-HTTP endpoint and optional headers (e.g. `Authorization`) |

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_HOME` | Moves the data directory used for logs (default `./.pu/`). Only logs follow it: `session.json` lives in the workspace's `.pu/`, and `agents.json` in that workspace's `.pu/` or `~/.pu/` |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve`. Defaults to the first existing of `./web`, `../share/pu/web`, `/usr/share/pu/web`, `/usr/local/share/pu/web` |
| `PU_SERVE_HOST` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |

### Logging

The console shows only `error` and `critical`; the file gets `PU_LOG_LEVEL` (default
`info`) and goes to `<data-dir>/logs/pu.log`, rotating at 5MB and keeping 3 files.

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
