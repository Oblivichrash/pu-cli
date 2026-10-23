# CLI and agent workflow: what is broken, and what to build

Written after configuring four agents in `~/.pu/agents.json` and driving them from the
command line. Every claim below was reproduced against the installed binary
(`pu 0.5.1`) in this workspace; the command that settled each one is quoted.

The evidence is left as it was found — it is why each change was made — and the status of
each item is marked where it appears and collected in the table below. Commands named in the
notes and in the code excerpts describe 0.5.1; what runs now is in
[ARCHITECTURE.md](../ARCHITECTURE.md) and [configuration.md](../configuration.md).

## What has landed since this was written

| Item | State | Where it went |
| --- | --- | --- |
| §2.1 `ask` exits 0 on failure | **done** | five statuses, `ARCHITECTURE.md#what-a-command-exits-with-and-what-it-prints` |
| §2.2 empty prompt costs a request | **done** | `--prompt ""` is a usage error, exit 1 |
| §2.3 one error printed twice | **done** | the executor records, the front end prints |
| §2.4 no stateless mode | **not done** | needs `--fresh`; `ask` still appends |
| §2.5 read-only commands touch the workspace | **not done** | `Shutdown()` still saves unconditionally |
| §2.6 a leading `/` cannot be sent | **not done** | |
| §2.7 no stdin prompt | **not done** | |
| §2.8 no agent list outside the chat | **done** | `pu agents [--json]`, sharing its list with `/agents` and `GET /api/agents` |
| §2.9 no config validation | **done** | `pu check`, which resolves `${ENV}`, the gateway's host and the sandbox |
| §2.10 irrelevant flags pass silently | **done** | flags are held against the command's own page |
| §2.11 one `--help` for every command | **done** | `pu <cmd> --help` prints that command's page |
| §2.12 nothing reports cost or capability | **done** | `ask --show-usage` for a turn, `models --caps` for the choice between two models |
| §2.13 `(active)` misses the running model | **done** | `(active, not listed by this gateway)` |
| §1 `--agent` writes the session | **not done** | deliberately: it is a breaking change, see §4 |

Still open, in the order I would take them: §2.4 and §2.7 (both small, and together they make
`pu ask` usable from a script without touching the workspace), then §1 and §2.5 together (they
are one question: who may write the session).

---

## 1. The one design flaw underneath most of the symptoms

The CLI has one field answering two different questions:

```
"which agent does this process run"      — per invocation, should not outlive it
"which agent does this workspace remember" — per workspace, must outlive it
```

Both are `Runtime::default_agent_override_`. It is set by `--agent`
(`cli.cpp:37`, through `WithInitializedRuntime`), read at `runtime.cpp:113` to pick the
active agent, and then **written into the session** at `runtime.cpp:147-149`:

```cpp
const std::string remembered = agent_chosen_here ? active_agent : spec.agent_name;
const auto* stored = agent_manager_->AgentConfig(remembered);
...
spec.agent_name = stored->name;      // ← the per-run flag becomes persistent state
```

`Shutdown()` then saves that (`runtime.cpp:160-162`). So `--agent` is documented as
"agent to use" and behaves as "agent to use from now on".

### What that produces

| Reproduced | Command | Result |
| --- | --- | --- |
| A one-shot flag re-points the workspace | `pu ask --agent deepseek-v4-pro "say pong"` | `session_spec.agent_name` became `deepseek-v4-pro`, and stayed |
| A **read-only** query re-points it too | `pu models --agent deepseek-v4-pro` | same, and the next bare `pu ask` ran on `deepseek-v4-pro` |
| A **failed** run writes it | `PU_AGENTS_CONFIG=<unreachable> pu ask "hi"` | session rewritten, `agent_name: x` |

The second row is the sharp one: listing another agent's models silently changes which
agent your session talks to. It is not obvious from `--agent`'s help text, and the
session file is the only place it shows up.

### The mirror-image problem

Because the session wins over configuration (`runtime.cpp:143`), editing
`default_agent` in `agents.json` has no effect once a session file exists:

```
session agent = codebuddy-glm-5.3-flash, config default_agent = codebuddy-deepseek-v4-flash
$ pu ask "/backend"
Current backend: codebuddy (model: glm-5.3-flash, ...)      ← config ignored
```

This is documented as intentional (`ARCHITECTURE.md`: "the named agent wins over
`default_agent`"), and it is defensible. But it means renaming an agent — which is what
we just did — requires hand-editing `.pu/session.json`, or accepting one warning line and
a silent fallback. Neither is a workflow.

---

## 2. Defects, with evidence

### 2.1 `pu ask` exits 0 when it failed

**Fixed.** `RunAsk` answers with the status the turn reached: a failed turn is
2, and the five statuses are in
[ARCHITECTURE.md](../ARCHITECTURE.md#what-a-command-exits-with-and-what-it-prints).

`WithInitializedRuntime` returns 0 unless an exception escapes; `RunAsk`'s body reports
`result.has_error` and returns nothing (`cli.cpp:85-103`). So a failed turn is
indistinguishable from a good one to any caller:

```
$ PU_AGENTS_CONFIG=<unreachable> pu ask "hi"   →  exit 0
$ pu ask "/zzz"                                →  exit 0
$ pu ask --prompt ""                           →  exit 0
$ PU_AGENTS_CONFIG=<unreachable> pu models     →  exit 1   ← inconsistent
$ PU_AGENTS_CONFIG=<unreachable> pu probe      →  exit 1
```

`pu ask` is the command people put in scripts and `&&` chains. This is the single highest
value fix in this document.

### 2.2 An empty prompt costs a real request

**Fixed.** `--prompt ""` is refused as a usage error and no request is made.

`main.cpp` gates on `vm.count("prompt")`, and `--prompt ""` counts. `ProcessInput("")`
is not a command, so the executor runs a turn with empty input. Reproduced: it answered,
and appended nodes to the session.

### 2.3 The same error is printed twice

**Fixed.** The executor records `error_message`; the front end is what prints it.

```
$ PU_AGENTS_CONFIG=<unreachable> pu ask "hi"
[...] [error] Request failed: connect: Connection refused [...]
[...] [error] Request failed: connect: Connection refused [...]
```

`executor.cpp:289-290` sets `result.error_message` *and* logs it; `cli.cpp:92` logs it
again. The executor should record and let the front end decide, which is what the rest of
`ExecutionResult` already does.

### 2.4 Every `ask` appends to the workspace conversation

There is no stateless mode. Three consecutive runs in a fresh directory:

```
nodes after run 1: 4
nodes after run 2: 6
nodes after run 3: 8
```

So `pu ask` is "continue this workspace's conversation with one more turn", not "ask one
question". For a scripted probe that is both a correctness problem (the model sees
history it did not expect) and a growth problem (nothing prunes it).

### 2.5 Read-only commands still touch the workspace

`pu models` in an empty directory leaves:

```
.pu/logs-<pid>/pu.log
.pu/session.json
```

and rewrites `session.json` even when the bytes are identical (mtime moves, md5 does not).
Harmless in a repo, noisy in `git status` for anyone who tracks `.pu/`, and it is what
carries 2.1's agent switch.

### 2.6 A prompt starting with `/` cannot be sent

`ProcessInput` treats any leading `/` as a command (`runtime.cpp:251`), and `ask` has no
escape:

```
$ pu ask "/etc/hostname 这个文件是什么"   →  Request failed   (parsed as a command)
$ pu ask -- "/etc/hostname 这个文件是什么" →  Request failed   (same)
```

Asking about a path is an ordinary thing to do.

### 2.7 No stdin prompt

```
$ echo "say pong" | pu ask   →  Error: prompt required for ask command   (exit 1)
$ echo "say pong" | pu ask -  →  sends the literal string "-"
```

So `pu ask` cannot take a file or a pipeline: `pu ask "$(cat task.md)"` is the only
route, and it breaks on argv length and on content with quotes.

### 2.8 There is no way to list agents outside an interactive session

**Fixed.** `pu agents`, whose list the chat and the API now share.

`/agents` exists in chat (`command_router.cpp:198`) and `GET /api/agents` in serve, but
the CLI has no equivalent. The only non-interactive way to learn the configured names is
to fail on purpose:

```
$ pu ask --agent nope "x"
[error] Requested agent is not configured: nope; configured: codebuddy-deepseek-v4-flash ...
```

With four agents and names up to 27 characters, that is not a discovery mechanism.

### 2.9 There is no config validation

**Fixed.** `pu check` resolves the file and prints what it found.

Nothing loads `agents.json` without also doing work. No `--check`, no `--dry-run`, no
"print the resolved backend". Typos in `forbidden_patterns` or a gateway name are found by
running a turn.

### 2.10 Options are global, so irrelevant flags pass silently

**Fixed.** A flag belongs to the commands that read it, and is refused elsewhere.

One `options_description` holds every option (`cli.cpp:48-78`), so any subcommand accepts
any flag and ignores it:

```
$ pu models --host 1.2.3.4      → exit 0, host ignored
$ pu ask --no-save "hi"         → exit 0, flag ignored
$ pu ask --out /tmp/x "hi"      → exit 0, flag ignored
```

Unknown flags do error (`--bogus` → "unrecognised option"), so this is specifically the
"valid flag, wrong command" case — the one a user is most likely to hit after reading
`--help` once.

### 2.11 `--help` is one page for five commands

**Fixed.** `pu <cmd> --help` prints that command's page.

`pu ask --help` and `pu models --help` print the identical global page. There is no way to
ask "what does `ask` take".

### 2.12 Nothing reports cost or capability

**Fixed.** `ask --show-usage` prints what a turn cost, and `models --caps` prints the columns
that decide between two models — including `only-reasoning`, the flag that predicts a refused
request. `models --verify` exits 2 when a model it asked did not answer.

`ExecutionResult` carries `usage` (prompt / completion / cached / credit) and the CLI
never prints it. `ModelEntry` carries `supports_tools`, `supports_images`,
`supports_reasoning`, `only_reasoning`, `default_effort`, `max_input_tokens`, and
`FormatModelList` (`model_listing.cpp:190-205`) prints only the id and `(active)`.

Two consequences we hit directly while configuring:

- The 3x credit spread between `glm-5.3-flash` (x0.06) and `deepseek-v4-flash` (x0.17) is
  invisible from the CLI; it only exists in the gateway's catalogue.
- `only_reasoning` is exactly the flag that predicts the `11150` failure class. It is
  parsed, stored, and never shown.

### 2.13 `(active)` is wrong for the model that is actually running

**Fixed.** The running model is printed as `(active, not listed by this gateway)`.

`pu models` marks the active model by matching `backend.model` against the listing. For
CodeBuddy the listing is `data.agents[].models[]`, and `deepseek-v4-flash` is not in it:

```
$ pu models --agent codebuddy-deepseek-v4-flash | grep -c '(active)'
0
```

The running model is the one row that is not shown.

---

## 3. Design

Ordered so that each step is shippable on its own and none requires the next.

### 3.1 Exit codes — do first

One contract for all subcommands:

| Code | Meaning |
| --- | --- |
| 0 | the command did what it was asked |
| 1 | the invocation or configuration is wrong (bad flag, bad agent, unreadable `agents.json`) |
| 2 | the provider refused or could not be reached (network, HTTP error, stream failure) |
| 3 | the sandbox refused (forbidden pattern, approval with no channel) |

1 and 2 are separated because a script's remedy differs: fix the config, or retry.
`RunAsk` returns `result.has_error ? 2 : 0`; the existing `models`/`probe` behaviour (1 on
a failed read) becomes 2 for consistency. `--prompt ""` becomes 1, not a request.

Tests belong in `tests/unit/test_options.cpp` beside the existing parse cases, driving
`RunAsk` with a mock client — the exit code is the observable, so it needs no new seam.

### 3.2 Split the two agent questions

Add to `Runtime`:

```cpp
void SetRunAgent(const std::string& name);   // this process only
void SetWorkspaceAgent(const std::string& name);  // what /backend and chat do
```

`SetRunAgent` sets `agent_manager_->SetActiveAgent()` and rebuilds the toolbox, and never
touches `Session::Spec()`. `WithInitializedRuntime` uses it, so `--agent` stops writing
`session_spec.agent_name`.

Then decide per subcommand whether the session is touched at all:

| Command | Session read | Session written |
| --- | --- | --- |
| `ask` | yes (continues the conversation) | yes |
| `ask --fresh` | no | no |
| `chat` | yes | yes |
| `models`, `probe`, `agents`, `check` | only to name the default agent | **never** |

`--fresh` is the stateless mode from 2.4: an empty conversation, nothing saved. That is
also the right default for `probe`, which exists to capture a gateway's raw stream and has
no business editing a conversation.

Not writing for read-only commands fixes 2.5 and the second row of §1 in one change, and
it is safe: nobody depends on `pu models` rewriting `session.json`.

### 3.3 `pu agents` and `pu check`

```
pu agents [--json]        # name, gateway, model, host, thinking — the table /agents shows
pu check [--config PATH]  # load and validate, print the resolved backend per agent
```

`pu agents` is `HandleAgents` lifted out of `CommandRouter`; `--json` reuses the shape
`GET /api/agents` already returns, so the two cannot drift.

`pu check` is the piece that would have saved time in this exercise: it prints what
`ParseAgentEntry` actually resolved — gateway, host after `DefaultHostFor` and
`${ENV}` expansion, model, thinking, sandbox root after `ResolveWorkspacePath` — and
warns on keys the parser ignores (`tools`, `history_compaction` were both silently dead).
It exits 1 on a load failure and 0 otherwise, with no network call.

An ignored-key warning is worth adding to `ParseAgentEntry` itself rather than only to
`check`: a config carrying a field this build does not read is a mistake in every
invocation, not only the ones that ask.

### 3.4 Scope options to their subcommand

Build one `options_description` per subcommand and add only that one to the parser,
keeping `--help` and `--version` global. Then `pu models --host` is "unrecognised
option", which is what it is.

`BuildCommandLine` already exists to keep the names in one place and is tested by
`test_options.cpp` ("The command line names every option once"); the per-command split
extends that test rather than replacing it.

`pu <cmd> --help` then prints that command's page. Cheap, and it is what makes 3.5 and
3.6 discoverable.

### 3.5 Per-run overrides

The chat already has `/thinking`; the CLI cannot express it. Add to `ask` and `chat`:

```
--thinking none|low|medium|high|auto   # auto = follow agents.json
--temperature F
--max-tokens N
```

These are the fields people actually tune per run, and they exist in `Backend` already —
this is a front end for `SetThinkingLevel` (`runtime.cpp:234`) plus a transient `Backend`,
not new state. It also gives `pu models --verify` a way to answer "does this model work at
`high`", which is the question `only_reasoning` makes people ask.

### 3.6 Show cost and capability

- `ask`: print `usage` on stderr when `--show-usage` is given (stdout stays clean for
  piping). Tokens plus `credit` when the gateway reported it.
- `models`: add `--caps` to print the columns `Capabilities` already holds —
  `tools`, `images`, `reasoning`, `only-reasoning`, `default-effort`, `max-input`.
  A `only-reasoning` model with no `thinking` configured is exactly the row a user needs
  warned about.
- `models`: when the configured model is absent from the listing (2.13), print the model
  once with `(active, not listed)` instead of dropping the marker. The listing is the
  gateway's subset; the running model is a fact the CLI knows regardless.

### 3.7 Prompt input

- `pu ask -` reads the prompt from stdin; `pu ask` with no prompt and a non-tty stdin does
  the same. Files and pipelines then work without `$(cat ...)`.
- A leading `/` is a command only in `chat`, where commands exist. In `ask`, `/foo` is a
  prompt. If the escape is wanted anyway, `--` should mean "everything after this is the
  prompt", which is what `--` means everywhere else.

The first is additive; the second changes behaviour, so it needs the release note
`README.md` already keeps for config changes.

### 3.8 Agent name ergonomics

Names are now up to 27 characters and get typed at `--agent`, `/backend` and the Web
selector. Two cheap options, either of which removes most of the typing:

1. **Unique-prefix match** in `AgentConfig`: `--agent cb-glm` resolves if exactly one
   configured name starts with it, and errors listing the candidates if more than one
   does. No schema change, no new state.
2. **An `alias` field** per agent, resolved before the name lookup. Explicit, but it adds
   a config key and a second namespace to keep unique.

Recommendation: (1). It needs no configuration and it fails loudly on ambiguity. (2) can
come later if someone wants a genuinely different name.

---

## 4. What not to change

- **The session outranking `default_agent`** is correct. A user who typed
  `/backend deepseek-v4-pro` yesterday expects that today. The fix for "renaming an agent
  is painful" is `pu check` telling you the session names an unknown agent and offering
  the names that exist — not reversing the precedence.
- **`ask` continuing the conversation by default.** It matches `chat` and it is what makes
  `pu ask` useful for follow-ups. `--fresh` covers the other case.
- **Options in one place.** `BuildCommandLine` keeping every name in one function is what
  makes "a name registered twice" catchable; 3.4 splits the *descriptions*, not that
  property.
