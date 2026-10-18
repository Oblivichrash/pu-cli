# Release notes

Notable changes, newest first. This file is a summary; a change's reasoning is in
the commit that made it, and its current behaviour is in the relevant document
(see [Where Things Live](CONTRIBUTING.md#where-things-live)).

## Unreleased

### Added

- Peer messaging: a `send_to_peer` tool, `POST /api/peer/message`, and
  `.pu/peers.json` with `PU_PEER_TOKEN` overriding the shared secret
  ([docs/configuration.md](docs/configuration.md#peersjson), [docs/web-api.md](docs/web-api.md)).
- A status bar reading the session at the shell's width.
- The Web panel's Session block holds the agent and thinking selects.

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
