# Configuration

Every field `pu` reads. Where a value belongs to the build rather than to a file (a
gateway's default host, the variable a key is read from), it is a fact of the gateway
table in `src/gateways/`, not of this document.

## `agents.json`

The file lives in a `.pu/` directory: `./.pu/agents.json` (project) or
`~/.pu/agents.json` (user, resolved from `HOME`, which Windows does not set by
default). See [Configure](../README.md#configuration) for a full example.

**Top-level fields**

| Field | Description |
|-------|-------------|
| `default_agent` | Required; must name one of the entries in `agents` |
| `agents` | Required; the entries below |
| `serve` | Optional; `{"bind": ..., "port": ...}` for `pu serve`, used for whichever of the two neither the command line nor the environment names. `bind` is the interface the listener sits on, not an address to dial |

**Agent fields**

| Field | Description |
|-------|-------------|
| `name` | Identifier used by `default_agent`, `/backend <agent>`, and `--agent` |
| `description` | Optional text shown when the agent connects |
| `backend` | Required object; see below |
| `security` | Optional; see below |
| `mcp_servers` | Optional; see below |
| `tool_call_budget` | Optional; how many tool calls the model may make in one turn (default 100). The count is over calls, not replies, and it is checked between rounds: a budget of 2 lets two calls happen and then asks the model to wrap up. A wrap-up that calls a tool anyway ends the turn with an error naming this setting. A session may override it from the page without touching this file |

**Backend fields**

| Field | Default | Description |
|-------|---------|-------------|
| `gateway` | — | Required. One of the rows in [providers.md](providers.md#which-provider-a-request-reaches); a missing or unknown name is refused rather than defaulted, so a backend never reaches a host its author did not choose |
| `host` | per gateway | Base URL; omitted, it falls back to the gateway's own `default_host`, which the load error and [providers.md](providers.md) both answer |
| `model` | — | Required |
| `api_key` | unset | Sent as `Authorization: Bearer` when set |
| `temperature` | `0.7` | |
| `max_tokens` | unset | A cap on the reply. Unset sends no cap, leaving the provider's own limit to apply. A cap reached while a tool call is being written ends the turn with an error, since half an argument would run the tool on a partial value |
| `thinking` | `default` | `none`, `low`, `medium`, `high`, or `default`. Set it per session with `/thinking`, the Web header, or `POST /api/settings` with `{"name":"thinking","value":"low"}`. What each level puts on the wire is in [providers.md](providers.md) |
| `system_prompt` | unset | Merged with the injected context |

`codebuddy` is the `openai` protocol against the CodeBuddy cloud gateway; the
gateway's contract is in
[design/codebuddy-cloud-api.md](design/codebuddy-cloud-api.md). Its
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

### Reading it back: `pu check`

`agents.json` is not the values that run. Three steps stand between the two and none of them
shows anywhere: `${ENV}` is expanded, a gateway supplies the host a backend did not name, and
a relative `sandbox_root` is resolved against the workspace. Each can be wrong while the file
looks right — an unset variable becomes an empty key that the provider answers with 401, and a
host that came from the wrong gateway fails as a refused connection.

`pu check [--config PATH]` prints the resolved values without contacting a provider:

```bash
pu check                          # ./.pu/agents.json, then ~/.pu/agents.json
pu check --config /tmp/other.json
```

```
config  /home/me/.pu/agents.json

workspace  /home/me/work
default    fast

fast   (default_agent)
  gateway      codebuddy
  host         https://copilot.tencent.com/v2   (default for this gateway)
  model        deepseek-v4.1-flash
  api_key      (set, 59 characters)
  thinking     default
  sandbox      /home/me/work
  forbids      'sudo', 'mkfs'
  tool budget  100
```

A host marked `(default for this gateway)` is one the file never named. A key printed as
`(empty)` is a reference that expanded to nothing, and the warning beside it names the
variable. A field this build never reads is listed under `warnings` — a build may be older
than the file, so that is a warning and not a failure.

A session naming an agent the file no longer has is reported with the names that do exist.
The session outranks `default_agent` by design (see
[ARCHITECTURE.md](ARCHITECTURE.md#persistence)), so this is a fact to read rather than a file
to fix.

The command exits `1` only when the file cannot be loaded — which is what the 0.5.0 rename of
`backend.type` to `backend.gateway` produces, without spending a turn to find out. The status
table is in [ARCHITECTURE.md](ARCHITECTURE.md#what-a-command-exits-with-and-what-it-prints).

## `peers.json`

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

`send_to_peer` posts a job on the receiving server's board before it says anything, so
the hand-off exists as an object before it exists as a message: the job names the peer it
is for, the `intent` it was asked to do and an optional `due` duration, and the receiver
claims it as it accepts the turn. A board that refuses the job stops the call, so work the
receiver could not take on never arrives as a turn. When the turn ends the job closes with
`done` and the answer, or `failed` and the error, which is how the sender's next turn — or a
person reading `/api/jobs` — learns the outcome without polling the peer.

The call returns as soon as the peer accepts rather than waiting for an answer, which
arrives as a new turn. A turn that arrived from a peer may reply to that peer and to nobody
else, so an A→B→A→B chain ends at the second hop by construction rather than by a counter.

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Names the `agents.json` to load, ahead of `./.pu/agents.json` and `~/.pu/agents.json`. A path that does not exist is an error rather than a fallback |
| `PU_HOME` | Moves the log directory (default `./.pu/logs/`). Only logs follow it: `session.json` lives in the workspace's `.pu/`, and `agents.json` in that workspace's `.pu/` or `~/.pu/` |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve`. Ahead of the page directory that ships with the binary, which is found under the executable's own prefix, then `./web` and the system install locations |
| `PU_SERVE_BIND` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |
| `PU_PEER_TOKEN` | Overrides `token` in `peers.json`, so the secret can stay out of the file |

## Logging

The console shows only `error` and `critical`; the file gets `PU_LOG_LEVEL` (default
`info`) and goes to `<data-dir>/logs/pu.log`, rotating at 5MB and keeping 3 files.

The writer behind the file is an async logger on a single thread pool. That pool is
created once per process and never replaced, so a second `Runtime` — a retried server, a
second `pu serve` in the same process — builds a new logger on the pool the first one is
still writing through. Rebuilding the pool instead would leave the earlier logger posting
into a queue that no longer exists.

