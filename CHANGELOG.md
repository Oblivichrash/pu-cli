# Release notes

Notable changes, newest first. This file is a summary; a change's reasoning is in
the commit that made it, and its current behaviour is in the relevant document
(see [Where Things Live](CONTRIBUTING.md#where-things-live)).

## Unreleased

### Added

- `pu check [--config PATH]` prints what an `agents.json` resolves to without contacting a
  provider: the host a gateway supplied, the length an `${ENV}` reference expanded to, the
  directory a relative `sandbox_root` becomes, and every field this build does not read. A
  file it cannot load exits 1; a warning is not a failure.
- Peer messaging: a `send_to_peer` tool, `POST /api/peer/message`, and
  `.pu/peers.json` with `PU_PEER_TOKEN` overriding the shared secret
  ([docs/configuration.md](docs/configuration.md#peersjson), [docs/web-api.md](docs/web-api.md)).
- A status bar reading the session at the shell's width.
- The Web panel's Session block holds the agent and thinking selects.

### Changed

- **The CLI has exit codes** (`docs/design/cli-agent-workflow.md` §3.1). `0` did what it
  was asked, `1` the invocation or configuration is wrong, `2` the provider refused or
  could not be reached, `3` the sandbox refused, `4` was interrupted. `pu ask` used to
  return 0 for a failed turn; it now returns 2, and `pu models --verify` returns 2 when a
  model it asked did not answer.
- **Diagnostics moved to stderr.** A notice, a usage line and an error are printed on
  stderr; stdout carries only what the command was asked for, so `pu ask "..." | ...` reads
  the reply and nothing else.
- A failed turn is printed once. The executor records `error_message` and the front end
  prints it; both were printing the same line.
- `--prompt ""` is refused as a usage error instead of being sent as a turn.
- `pu <cmd> --help` prints that command's page instead of one page for all of them, and a flag
  another command owns is refused as unrecognised before anything runs. The whole line was
  what the parser saw, since which command was typed is only known after parsing; the flags
  are now held against the command's own page once it is.
- `pu ask` answers 3 when the sandbox refused every tool call the turn made and no answer
  came back, and 4 when a person interrupted it. A turn that a refusal did not stop keeps
  answering 0: the model read the refusal and ran something else.
- A turn that could not run because nothing could ask for the approval its commands need
  answers 1, not 3: the command is fine and the front end is what has to change. A tool
  result now names that refusal (`refusal: "nobody_to_ask"`) beside the `blocked` flag it
  already carried.

### Fixed

- A refresh no longer draws the last turn twice: the page has one loader for the transcript
  and holds the arriving frames until the rebuild finishes.
- The page no longer stops mid-answer. The hold on arriving frames is raised in one place and
  released on every path, including a timeout, so a rebuild that failed can no longer leave
  the transcript frozen until a reload.

## 0.5.1

### Changed

- A cancelled turn stores every tool call it never ran as `interrupted` rather than
  leaving it `pending`, so it no longer blocks `/rewind` or an agent switch.
  Sessions saved by 0.5.1 that still hold a `pending` tool call are read as written and
  keep refusing a rewind; start a new session.
- `PU_AGENTS_CONFIG` names an `agents.json` outright, ahead of
  `./.pu/agents.json` and the user configuration. A path that does not exist is an
  error rather than a silent fallback.

## 0.5.0

### Changed

- **Breaking**: `backend.type` was renamed to `backend.gateway`, and the old key is no
  longer read. A config still using `type` fails at load with
  `Missing gateway in backend: name one of ollama, openai, codebuddy`. Rename the one key
  per backend; the agent-level `"type": "llm"` is a different field and is unaffected.
- A gateway is a named product rather than a protocol: `deepseek`, `nim` and
  `ollama-cloud` join `ollama`, `openai` and `codebuddy` in the gateway table, and
  `pu models --gateway <name>` reads a product's own catalogue.
- `pu models --entry <id>` prints an `agents.json` entry ready to paste.
- Token usage and the context a turn ran against are carried to the Web UI.
