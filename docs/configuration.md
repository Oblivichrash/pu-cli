# Configuration

Every field `pu` reads. Where a value belongs to the build rather than to a file (a
gateway's default host, the variable a key is read from), it is a fact of the gateway
table in `src/config/gateway.cpp`, not of this document.

## `agents.json`

The file lives in a `.pu/` directory: `./.pu/agents.json` (project) or
`~/.pu/agents.json` (user, resolved from `HOME`, which Windows does not set by
default). See [Configure](../README.md#configuration) for a full example.

**Top-level fields**

| Field | Description |
|-------|-------------|
| `name` | Required; what identifies this agent in `pu check`, `/backend` and the session |
| `serve` | Optional; `{"bind": ..., "port": ...}` for `pu serve`, used for whichever of the two neither the command line nor the environment names. `bind` is the interface the listener sits on, not an address to dial |

The file describes one agent. The fields below are that agent's, at the top level of the file;
`backend` is the one that must be there.

**One agent, no table**

```json
{
  "name": "default",
  "backend": { "gateway": "ollama", "model": "qwen2.5:0.5b" }
}
```

`name` is required: a turn, a session and a `/backend` all name the agent they mean, and a file
that does not name itself leaves nothing to name. A file with no `backend` is refused with
`Missing backend`: it describes an agent, and the backend is the part a turn cannot be sent
without.

`pu setup` writes this form, so a workspace configured by hand and one configured by the
wizard are the same shape.

**Agent fields**

| Field | Description |
|-------|-------------|
| `name` | Identifier used by `/backend <agent>`, `--agent`, and the session |
| `description` | Optional text shown when the agent connects |
| `backend` | Required object; see below |
| `security` | Optional; see below |
| `mcp_servers` | Optional; see below |
| `tool_call_budget` | Optional; how many tool calls the model may make in one turn (default 100). The count is over calls, not replies, and it is checked between rounds: a budget of 2 lets two calls happen and then asks the model to wrap up. A wrap-up that calls a tool anyway ends the turn with an error naming this setting. A session may override it from the page without touching this file |
| `tools` | Optional; which builtin tools this agent has, by name: `bash`, `write_file`, `ask_dir`. Absent means all of them, so an agent meant to be read-only has to say so. A name this build does not register is refused at load |
| `delegates` | Optional; the agents this one may hand a turn to, by name. Absent means it may hand a turn to nobody. `pu check` refuses this agent's own name, since the turn that reached it would recurse until a budget stopped it. A name nothing configures is found when `ask_dir` is called, not here: the agent that would answer may be a directory this workspace does not hold, so a file is in no position to say it does not exist. A circle is refused when it closes, by the child that would be asked to answer a question already being worked on further up the chain |
| `system_prompt` | Optional; this agent's system prompt, merged with the injected context |

**Backend fields**

| Field | Default | Description |
|-------|---------|-------------|
| `gateway` | — | Required. One of the rows in [providers.md](providers.md#which-provider-a-request-reaches); a missing or unknown name is refused rather than defaulted, so a backend never reaches a host its author did not choose |
| `host` | per gateway | Base URL; omitted, it falls back to the gateway's own `default_host`, which the load error and [providers.md](providers.md) both answer. Editable from the Web page's configure dialog, which writes this key back as typed: `${VAR}` is kept as spelled rather than expanded, and an empty field restores the default rather than pinning an empty address |
| `model` | — | Required |
| `api_key` | unset | Sent as `Authorization: Bearer` when set |
| `temperature` | `0.7` | |
| `max_tokens` | unset | A cap on the reply. Unset sends no cap, leaving the provider's own limit to apply. A cap reached while a tool call is being written ends the turn with an error, since half an argument would run the tool on a partial value |
| `thinking` | `default` | `none`, `low`, `medium`, `high`, or `default`. Set it per session with `/thinking`, the Web header, or `POST /api/settings` with `{"name":"thinking","value":"low"}`. What each level puts on the wire is in [providers.md](providers.md) |
| `context_window` | unset | How much context the model holds, in tokens. Advisory: nothing trims a prompt against it and no turn is refused for outgrowing it. It is the only figure the Web page's usage reading measures the last reply against, so a model without it shows the count alone with no hairline under it. `pu models --detail caps` prints what the gateway's own catalogue says, which is where a number to write here comes from |
| `system_prompt` | unset | Merged with the injected context |

### Asking a directory that is not an agent of yours

`ask_dir` takes one `ask`: a name from the table's `delegates`, or a **directory relative to
this one**. The second form reaches a `.pu/` this workspace does not own — its own agents table,
its own session, its own sandbox — which is what makes it useful for a sub-agent created
mid-conversation.

Three things are true of it, and the first is the one that surprises:

- **The directory must already carry its own `.pu/agents.json`.** Nothing creates it: a
  directory without one is refused, not configured on the fly. The refusal names the file and
  the shape to write, so the recovery is one `write_file` followed by the same question again.
- **The path is under here.** It is resolved against this workspace's sandbox root and must
  stay inside it; an absolute path is refused with the leading component it wants dropped,
  since an absolute path is the shape of a mistake rather than a request.
- **The turn is one hop.** The delegated agent has no `ask_dir` of its own, so a question
  cannot travel further than the agent it was handed to. This is the current implementation
  rather than a depth limit to tune: the child's toolbox simply never registers the tool, so
  nothing would carry over to a second hop.
- **A tool that needs approval fails inside a child.** A delegated `Runtime` has no approval
  gate, so any call its own `approval_patterns` cover comes back `kNobodyToAsk` even with a
  user at the page: the gate belongs to the parent's toolbox. A tool you expect to be approved
  is not available to a sub-agent, and there is no flag that changes that. See
  [limitations.md](limitations.md) for why the boundary is drawn there.

Its sandbox is its own, taken from its own table, so what it may read is decided there and not
here; a sub-agent whose table is wider than the parent's is a decision its `.pu/` made.

The `.pu/agents.json` a sub-agent reads is written by the conversation that asks it, which is the
point of the tool and also what its trust is worth: it is a file for this conversation, not a
reviewed policy. It is created by `write_file` and means nothing outside the turn that wrote it, so
do not treat a child's file as evidence that a repository agreed to anything. It is also not covered
by the built-in `agents.json` checks that `pu` runs against the workspace's own table, so a
malformed child file fails at the refusal, not at load.

`tencent` is the `openai` protocol against the CodeBuddy cloud gateway; the
gateway's contract is in
[design/codebuddy-cloud-api.md](design/codebuddy-cloud-api.md). Its
supported models are the account's, so a request naming one the account may not
use is answered by the gateway itself.

```json
"backend": {
  "gateway": "tencent",
  "host": "https://copilot.tencent.com/v2",
  "model": "deepseek-v4-flash",
  "api_key": "${TENCENT_API_KEY}"
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

**`paging`**

What a session keeps of a command's output, and how much of it one page shows. One
block, one owner: the pager reads these, and the tool that writes to it obeys them.
The shape a page takes is in [paging.md](paging.md).

| Field | Default | Description |
|-------|---------|-------------|
| `page_lines` | `80` | Lines a page shows when the call names no `limit`; a value below 1 is read as 1. Chosen to match about two screens of terminal scrollback |
| `max_lines` | `400` | The most lines one page may show — a larger `limit` is clamped to it. Raised to `page_lines` when set below it |
| `page_bytes` | `16384` | Bytes one page may show. Lines and bytes do not track each other, so a page is bounded by both and a page of packed machine output is cut short by this. Raised to 1024 when set below it |
| `capture_bytes` | `262144` | Bytes of one command's output kept for paging; beyond it the command still runs and the rest is counted rather than kept. Raised to 1024 when set below it |
| `retain_bytes` | `4194304` | Bytes kept across all retained runs; the oldest are dropped first |
| `retain_buffers` | `8` | Runs kept readable. A cursor whose run has been dropped is refused, not pointed at a neighbour |

The budget buys lines for the end the call asked for, so a model reading a log can size
`capture_bytes` for it without that costing a file's opening bytes.

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
agent      fast
session  /home/me/work/.pu/session.json
  names  fast

fast
  gateway      tencent
  host         https://copilot.tencent.com/v2   (default for this gateway)
  model        deepseek-v4.1-flash
  api_key      (set, 59 characters)
  thinking     default
  context      250000
  sandbox      /home/me/work
  forbids      'X1', 'X2'
  tool budget  100
```

A host marked `(default for this gateway)` is one the file never named. A key printed as
`(empty)` is a reference that expanded to nothing, and the warning beside it names the
variable. A field this build never reads is listed under `warnings`, named with the path it
sits at — the agent itself, `backend`, `security` or `paging` — because a build may be older
than the file, and that is a warning and not a failure.

A session naming an agent the file does not name is reported as a warning. The file has one
agent, so the next turn uses that one and the session is corrected rather than the file (see
[runtime.md](runtime.md#single-session-auto-persistence)).

The command exits `1` only when the file cannot be loaded — which is what the 0.5.0 rename of
`backend.type` to `backend.gateway` produces, without spending a turn to find out. The status
table is in [errors.md](errors.md).

### Writing it the first time: `pu setup`

A workspace with no `agents.json` has nothing a turn can be sent with, and three of the facts
the file holds — gateway, address, model — are the reader's own account rather than anything
this build can guess. `pu setup` asks for them and writes the file:

```
No agents.json here yet. Three questions and this workspace is ready.

Which gateway?
  1) ollama
  2) ollama-cloud
  3) openai
  4) deepseek
  5) nvidia
  6) tencent
  7) llamacpp
  8) anthropic

Gateway [ollama]: 
URL [http://localhost:11434]: 
What this gateway offers:
  1) qwen2.5:0.5b          tools max-in=32768
  2) llama3.1:8b           tools max-in=131072

Model (number or id): 1
Wrote .pu/agents.json
```

The menu is the gateway table itself, in the order it is written, so a gateway added to
`src/llm/gateway_table.cpp` is one the first run offers — `ollama-cloud`, `deepseek` and
`nvidia` were missing from a hand-maintained list of four. The list names the products and
nothing else: which variable holds a gateway's key is a question the next prompt answers, and
printing it twice made the menu a paragraph where a list was wanted.

The gateway prompt shows the first entry as its default, so `<enter>` takes it and a name or
number typed instead replaces it.

Every question follows the same shape, and it is the shape a shell prompt has: the default is
shown in brackets after the question, a blank line takes it, and anything typed is the answer.
`URL [http://localhost:11434]:` is also what a script sees, so the answers a script gives are
the answers a reader gives. The terminal is never put in a mode of its own — no raw mode, no
line editing here — so the reader's own keymap, history and screen reader all work as they do
everywhere else on the machine. `^C` or `^D` abandons the wizard, which writes nothing.

It asks in the order a reader can answer: the gateway first, because it decides both the
default address and whether a key is needed at all; then the URL, where blank means the
gateway's own; then the key, asked only when the gateway has one to ask for. The catalogue is
then fetched with the answers so far and printed with numbers beside what the catalogue says
about each model — the same capability line `pu models --caps` prints, including `tools`,
`images`, `reasoning`, a forced `effort`, and the input window. A model the catalogue says
nothing about is marked `(nothing stated)` rather than left blank, because a blank column reads
as one that failed to print. The number is a keystroke where the id would be a spelling.

The key question names the variable the gateway reads, and blank there means that variable:
the catalogue is listed with it, and the written file names no key at all, so the secret stays
in the environment where it already was. Only a variable that is unset is refused — nothing
would answer the question then, and the turn would fail on its first request somewhere far
from this prompt. A key typed in is written to the file.

A listing that fails is not the end of the run: the model can be typed, which is what a
machine with no route to the gateway needs. A blank model is refused, because there is nothing
to write. Prompts go to stderr and the file is written to `./.pu/agents.json` — the same
single-agent form shown above — and an existing file is never replaced.

**The first run asks these questions itself.** `pu ask`, `pu chat` or `pu serve` with no
configuration and a terminal attached runs the same wizard before the command, so the command
that needed a file runs against the one just written, once, with no second invocation. A run
with no terminal — a script, a pipe, CI — is asked nothing and gets the usual "Configuration
file not found"; a wizard cannot read a keyboard that is not there. `pu setup` is the explicit
way to ask for the questions when a file already exists or when the answer must be written by
hand.

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Names the `agents.json` to load, ahead of `./.pu/agents.json` and `~/.pu/agents.json`. A path that does not exist is an error rather than a fallback |
| `PU_HOME` | Moves the log directory (default `<workspace>/.pu/logs/`). Only logs follow it: `session.json` lives in the workspace's `.pu/`, and `agents.json` in that workspace's `.pu/` or `~/.pu/` |
| `PU_LOG_LEVEL` | File log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_WEB_DIR` | Directory served as the Web UI for `pu serve`. Ahead of the page directory that ships with the binary, which is found under the executable's own prefix, then `./web` and the system install locations |
| `PU_SERVE_BIND` | Overrides the Web server bind address for `pu serve` (default `127.0.0.1`) |
| `PU_SERVE_PORT` | Overrides the Web server port for `pu serve` (default `8080`) |

## Logging

The console shows only `error` and `critical`; the file gets `PU_LOG_LEVEL` (default
`info`) and goes to `<data-dir>/logs/pu.log`, rotating at 5MB and keeping 3 files.

The data directory is `$PU_HOME` when it is set, and otherwise the workspace's `.pu/`. It
is the workspace this run is about, not the directory the shell happened to be in: `pu -C
/tmp/proj` writes `/tmp/proj/.pu/logs/pu.log`. `pu setup` logs too, since the one thing
worth reading afterwards is why a file was refused or where it was written.

The writer behind the file is an async logger on a single thread pool. That pool is
created once per process and never replaced, so a second `Runtime` — a retried server, a
second `pu serve` in the same process — builds a new logger on the pool the first one is
still writing through. Rebuilding the pool instead would leave the earlier logger posting
into a queue that no longer exists.

