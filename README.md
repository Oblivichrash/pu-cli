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
ctest --test-dir build --parallel --output-on-failure
```

### Develop and install

The build tree's `pu` runs from the checkout and finds the page in `web/` beside it,
so day-to-day development needs no install step:

```bash
PU_WEB_DIR=web ./build/pu serve
```

An install puts the binary and the page under one prefix, and the installed binary
reads the page from the prefix it was installed into:

```bash
cmake --install build --prefix /path/to/pu-install   # bin/pu + share/pu/web/
```

Editing `web/` after an install means installing again — the installed copy is not
a live link into the checkout, and a binary running against its own prefix serves
the copy it was installed with.

Each test case is its own process, so the suite parallelizes without coordination. The
fixtures name their temporary directories after the process id for that reason; one that
reused a fixed name would delete another test's workspace mid-run.

### Configure

Create `agents.json` inside a `.pu/` directory — either `./.pu/agents.json` (project-level) or `~/.pu/agents.json` (user-level, resolved from `HOME`):

```json
{
  "default_agent": "local-assistant",
  "agents": [
    {
      "name": "local-assistant",
      "backend": {
        "gateway": "ollama",
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

> **Upgrading from 0.4.x?** `backend.type` was renamed to `backend.gateway`, and the
> old key is no longer read. A config still using `type` fails at load with
> `Missing gateway in backend: name one of ollama, openai, codebuddy`. Rename the
> one key per backend; nothing else changes. The agent-level `"type": "llm"` is a
> different field and is unaffected.

> **Upgrading from 0.5.0?** No configuration changes. `PU_AGENTS_CONFIG` is new: it names
> an `agents.json` outright, taking precedence over `./.pu/agents.json` and the user
> configuration, and a path that does not exist is an error rather than a silent
> fallback.

> **Upgrading from 0.5.1?** A turn that is cancelled now stores every tool call it
> never ran as `interrupted` rather than leaving it `pending`, and no longer blocks
> `/rewind` or a switch of agent. Sessions saved by 0.5.1 that still hold a `pending`
> tool call from a cancelled turn are read as they were written: they stay `pending`
> and keep refusing a rewind, because a `pending` record means a call may still be
> running. `pu chat` offers no repair; start a new session, or ask for one after that
> call has been answered.

### Usage

| Command | Description |
| :------ | :---------- |
| `pu ask <prompt>` | Send one prompt, print the reply, and exit |
| `pu chat` | Start an interactive session |
| `pu models [--verify]` | List the models the agent's backend reports, with the active one and its price marked; `--verify` asks each one for a token first (see below) |
| `pu models --entry <id>` | Print an `agents.json` entry for that model, ready to paste; nothing is written |
| `pu models --gateway <name>` | List a gateway's own catalogue, without an `agents.json` entry |
| `pu serve [--host H] [--port P]` | Start the Web UI (see [Web Server](#web-server-pu-serve)) |

Global options: `-h`/`--help` and `--version`. `ask`, `chat` and `models` also
accept `--agent <name>`; `ask` accepts `--prompt <text>` in place of the
positional prompt.

```bash
./build/pu ask "summarize README.md"
./build/pu chat --agent local-assistant
> /backend deepseek-pro    # switch to predefined agent
```

`pu models` reads the listing endpoint the backend's type names - `/api/tags` for
Ollama, `/models` for the OpenAI-compatible ones, `/v3/config` for CodeBuddy - and
prints what it names, with the active model marked and the gateway's own price
coefficient beside a model that reports one. A listing says a model is advertised;
it does not say the account may run it. `--verify` settles that by asking each listed model for a
single token and reporting what answered, one line per model, so an entitlement a
gateway would otherwise only report on first use is known up front. A model the
catalogue says is answered only with a reasoning level is reported rather than
asked when the backend sends none, since that request cannot succeed:

```bash
./build/pu models --verify
```

A gateway may report fewer models than the account may run - CodeBuddy answers with
the subset its own agent carries - so a model the backend is configured with can sit
outside the listing. It is named anyway, marked `(active, not listed by this
gateway)`, since it is the one row that matters most.

A listing this build cannot read is refused rather than reported as an empty one: a
response carrying no container the shape names fails with the top-level keys it did
carry, so "the gateway renamed something" never prints as "the account has no model".

`--gateway <name>` reads a product's own catalogue from the gateway table, so listing a
provider needs no configuration: its host and the variable its key comes from are the
table's facts, not the file's. The session is not touched, which is what makes this safe to
run while another agent is active. `--model` names the model beside it for a turn, since a
gateway names a host rather than a model.

```bash
./build/pu models --gateway nim
./build/pu models --gateway deepseek
./build/pu probe  --gateway nim --listing
./build/pu probe  --gateway deepseek --model deepseek-flash
```

The gateways a build carries are `ollama`, `ollama-cloud`, `openai`, `deepseek`, `nim` and
`codebuddy`. Two products on one protocol are still two entries: `deepseek` and `openai`
both speak OpenAI's wire format and answer at different hosts, which is why naming the
product is enough and naming the protocol is not.

`--entry <id>` renders the configuration for one model and prints it, so a new agent
is pasted rather than typed. It copies the running agent's own surroundings -
`gateway`, `host`, `api_key`, `temperature`, `max_tokens`, `security`,
`tool_call_budget`, `mcp_servers` - as the file spells them, so a `${VAR}` stays a
`${VAR}` and a file keeps one security policy rather than gaining a second. Two
fields are decided rather than copied: the entry is named `codebuddy-<model>` for a
gateway that proxies other vendors and `<model>` for a vendor's own API, and
`thinking` is filled from the catalogue when a model is answered only with a
reasoning level and the source names none. Nothing is written to disk.

```bash
./build/pu models --entry glm-5.3-flash
```

Your conversation is saved to `<workspace>/.pu/session.json` after every
interaction and restored on restart, so each directory keeps its own session.

A field this build reads but the reply never carried is reported after the answer,
not folded into a silent default: a gateway that renames `finish_reason` would
otherwise leave a truncated reply looking complete. A field the gateway *added* is
logged at warning rather than shown, since the standard stream metadata is unread on
every turn and a note that always appears is a note nobody reads.

`pu probe` sends one single-token prompt to the agent's backend, keeps every byte
of the raw stream under `--out` (default `./provider-probes/`), and prints the
field names each level of the reply actually carried. A `ResponseShape` for a
gateway is written from this capture rather than from a specification; the fields
it lists as absent are the ones an adapter for that gateway names differently or
drops:

```bash
./build/pu probe --agent local-assistant
./build/pu probe --no-save   # summary only, nothing written
```

`--listing` asks the same questions of the model catalogue instead of the stream: the keys
the answer carried at each level, and which of the names the catalogue shape reads it never
sent. A stream is read frame by frame and a listing is one object, so the two reports are
kept apart. Both read the endpoint the gateway's own lister reads, with the gateway's own
headers, so a listing that `pu models` can read is a listing `pu probe --listing` can
describe:

```bash
./build/pu probe --listing
```

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/backend <agent>` | Switch to a predefined agent (rebuilds the tool set) |
| `/backend <type> <model> [host] [api_key]` | Override the backend for this session, outranking `agents.json` for this run; the session keeps only the agent it names, and the fields are read afresh from `agents.json` on each start |
| `/agents` | List available agents |
| `/models [agent]` | List the models the active agent's (or a named agent's) backend reports |
| `/clear` | Clear conversation history |
| `/rewind <turn>` | Step back to before a turn; the next message replaces it |
| `/thinking [level]` | Show or set this session's thinking level (`auto` drops the session override and follows the agent's configuration again) |
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
  "blocked": true/false,
  "output": "...",
  "error": "...",
  "exit_code": 0
}
```

- `output` carries everything the command itself printed (a merged stream: the executor cannot tell stderr from stdout afterward)
- `error` is the tool layer's verdict — why the command failed, or why it was refused
- `blocked: true` marks a refusal by policy: the command never ran, so `exit_code` is `-1` and `output` is empty. The refusal names the forbidden pattern that matched and the sandbox's whole pattern list, so one denial teaches the model the full boundary

The executor reads `success` and `output`/`error` from it; the transcript keeps the result verbatim, so the model sees what the tool produced.

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

The front-end lives in `web/` as plain scripts with no build step, so the load order in
`index.html` is the dependency order and each file has one job — see
[ARCHITECTURE](docs/ARCHITECTURE.md#browser-modules). History is folded into one bubble per
turn, with each tool result keeping the output the stream showed. Under the composer it shows
the context the last reply ran against, from the counts the provider returned. No
window is shown beside it: none of the backends here reports one, and a guess would
read like a fact.

### Web API

Chat is a WebSocket at `ws://<host>:<port>/ws`. Everything else is REST, for control
and status. The page names itself on the upgrade (`/ws?client=<tab-id>`), so a reload
and a reconnect are recognisably the same page.

**Client → server**

```json
{"type":"run","payload":{"text":"user message"}}
{"type":"cancel"}
{"type":"note","payload":{"text":"words for the model at the next tool boundary"}}
{"type":"resolve_approval","payload":{"allow":true,"text":"optional note"}}
```

**Server → client**

```json
{"type":"chunk","payload":{"text":"token part"}}
{"type":"thinking","payload":{"text":"reasoning part"}}
{"type":"tool_start","payload":{"id":"call_1","name":"execute_bash","args":{"command":"ls"}}}
{"type":"tool_end","payload":{"id":"call_1","name":"execute_bash","args":{"command":"ls"},"output":"...","error":""}}
{"type":"notice","payload":{"text":"the reply stopped at the token limit..."}}
{"type":"usage","payload":{"prompt_tokens":48213,"completion_tokens":312}}
{"type":"busy","payload":{"text":"This session is already open in another page."}}
{"type":"done","payload":{"model":"gpt-4o-mini-2024-07-18"}}
{"type":"error","payload":{"text":"error description"}}
```

`usage` carries what the provider counted for the turn that just ended;
`prompt_tokens` is the context that turn ran against, so it grows with the
conversation and is what a page can show against a model's window. It arrives
before `done`, and a provider that reports no counts sends none.

`tool_start`/`tool_end` carry the tool call `id` so the UI pairs a result with
its call. The end carries the call's `name` and `args` as well, because a page
that reconnected mid-command is replayed the end without the start: what it draws
has to come from the end itself.

`note` delivers words without stopping anything, which the executor appends beside
the next tool result. A running command is stopped by `cancel`, which stops the turn
it belongs to: one button on the composer rather than one per command, since a
command is never the thing a reader wants to end on its own.

When the agent's policy names `approval_patterns`, a matching command sends
`approval_ask` before it runs and the turn thread parks until
`resolve_approval` answers. Allowed, the command runs and the note rides beside
its result; denied, the result is the refusal with the note in `error`. `cancel` sets the cancel token and leaves the connection open; the turn
then ends with `done`. `thinking` is a channel of its own, rendered beside the
answer; a backend that reports no reasoning sends none. `notice` is a reply that
is known to be incomplete (token limit or content filter) and is not an error.
`done` names the model that answered, which a gateway may have chosen.

A turn belongs to the client watching it. A `cancel` ends it with `done` and keeps what
already arrived. Closing the page or losing the connection ends it too, and what it had
written is not kept, so the session is left holding the question and no answer;
reconnecting then shows the conversation without the reply that was in progress.

A session has one page. While a page is attached, a second connection is answered with
`busy` and closed rather than taking over, since taking over would mean ending the
reply the page already there is reading. The comparison is the `client` name from the
upgrade: the same tab reconnecting takes the session back and the socket it left behind
is closed, while a different tab is refused. A client that names nothing is a page of
its own, so every connection from it is a second page.

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
| `POST` | `/api/peer/message` | Admit a message from a configured peer as a new turn (`{"from":"A","message":"...","hop":0}`); requires `Authorization: Bearer`, answers `202` rather than a reply |

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
| `tool_call_budget` | Optional; how many tool calls the model may make in one turn (default 100). When the budget is reached the model is asked to wrap up and answer from what it has gathered, rather than the turn ending in an error |

**Backend fields**

| Field | Default | Description |
|-------|---------|-------------|
| `gateway` | — | Required. `ollama`, `openai`, or `codebuddy`; a missing or unknown name is refused rather than defaulted, so a backend never reaches a host its author did not choose |
| `host` | per gateway | Base URL; omitted, it falls back to `http://localhost:11434` (ollama), `https://api.openai.com/v1` (openai), or the CodeBuddy gateway |
| `model` | — | Required |
| `api_key` | unset | Sent as `Authorization: Bearer` when set |
| `temperature` | `0.7` | |
| `max_tokens` | unset | A cap on the reply. Unset sends no cap, leaving the provider's own limit to apply. A cap reached while a tool call is being written ends the turn with an error, since half an argument would run the tool on a partial value |
| `thinking` | `default` | `none`, `low`, `medium`, `high`, or `default`. Set it per session with `/thinking`, the Web header, or `POST /api/thinking`. What each level puts on the wire is in [providers.md](docs/providers.md) |
| `system_prompt` | unset | Merged with the injected context |

`codebuddy` is the `openai` protocol against the CodeBuddy cloud gateway; the
gateway's contract is in
[docs/design/codebuddy-cloud-api.md](docs/design/codebuddy-cloud-api.md). Its
supported models are the account's, so a request naming one the account may not
use is answered by the gateway itself.

```json
"backend": {
  "gateway": "codebuddy",
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
| `sandbox_root` | All file operations are relative to this directory. Empty by default, which means the workspace itself |
| `forbidden_patterns` | Commands containing these substrings are blocked; include `"cd"` so the model cannot change the working directory |
| `max_command_length` | Reject a shell command longer than this many bytes. Defaults to `0`, which disables the check |
| `approval_patterns` | Commands matching one of these wait for the user's approval before they run; the Web UI shows the command with Approve and Deny buttons and an optional note, which reaches the model beside the result. `pu serve` only: the CLI has no channel to ask on, so a matching command there is refused |

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

### `peers.json`

Two `pu serve` instances can message each other: one agent calls `send_to_peer`, the
other answers in a later turn. A server with no `.pu/peers.json` does not mount the
endpoint and never registers the tool, so a single-server workspace needs nothing.

```json
{
  "self": { "name": "A", "host": "127.0.0.1", "port": 8911 },
  "token": "shared-secret",
  "peers": [{ "name": "B", "url": "http://127.0.0.1:8912" }]
}
```

| Field | Description |
|-------|-------------|
| `self.name` | This server's name, which the other side must list in its `peers` |
| `self.host` / `self.port` | Informational; the listener's own flags still decide where it binds |
| `token` | Shared secret every inbound message must present as `Authorization: Bearer` |
| `peers[].name` | The name this server knows a peer by, matched against the sender's `from` |
| `peers[].url` | Where that peer's `/api/peer/message` is posted |

Both sides must agree on the token and on each other's names. A peer entry without a
name and a `url` is skipped with a warning rather than trusted, and an undeclared
`from` is refused before any turn starts.

`send_to_peer` returns as soon as the peer accepts the message rather than waiting for
an answer, which arrives as a new turn. A turn that arrived from a peer may reply to
that peer and to nobody else, so an A→B→A→B chain ends at the second hop by
construction rather than by a counter.

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Names the `agents.json` to load, ahead of `./.pu/agents.json` and `~/.pu/agents.json`. A path that does not exist is an error rather than a fallback |
| `PU_HOME` | Moves the log directory (default `./.pu/logs/`). Only logs follow it: `session.json` lives in the workspace's `.pu/`, and `agents.json` in that workspace's `.pu/` or `~/.pu/` |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve`. Ahead of the page directory that ships with the binary, which is found under the executable's own prefix, then `./web` and the system install locations |
| `PU_SERVE_HOST` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |
| `PU_PEER_TOKEN` | Overrides `token` in `peers.json`, so the secret can stay out of the file |

### Logging

The console shows only `error` and `critical`; the file gets `PU_LOG_LEVEL` (default
`info`) and goes to `<data-dir>/logs/pu.log`, rotating at 5MB and keeping 3 files.

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
