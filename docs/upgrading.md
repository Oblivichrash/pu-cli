# Upgrading

What changed between versions for a workspace that already exists: the keys that moved,
the flags that were renamed, and what a session saved by an older build does when a newer
one opens it.

This list lives here rather than in [README](../README.md) because it only grows. A
README is read once, by someone with no configuration to migrate; this document is read
by someone whose configuration stopped working and who already knows which version they
came from. The full account of each release is in [CHANGELOG.md](../CHANGELOG.md).

## The two most recent

If a workspace is more than one release behind, read the whole table below rather than
this section — a migration step is only skippable when nothing after it depends on it.

| From | What to do |
|------|------------|
| 0.7.2 | `nim` is now `nvidia` and `codebuddy` is now `tencent`, and their keys are read from `NVIDIA_API_KEY` and `TENCENT_API_KEY`. The old spellings are not kept as aliases, so rename the gateway in any `agents.json` that names one and move the key to the new variable. |
| 0.7.0 | See below; two of its changes are silent if skipped. |

## Every version

| From | What to do |
|------|------------|
| 0.4.x | Rename `backend.type` to `backend.gateway`. The agent-level `"type": "llm"` is a different field and needs no change. |
| 0.5.1 | A cancelled turn now stores its unrun tool calls as `interrupted`. A session saved earlier that still holds a `pending` call keeps refusing `/rewind`; start a new session. |
| 0.6.0 | `pu sse` takes `--format text\|json\|raw` instead of `--json`/`--raw`. `pu models --entry` and `pu models --caps` moved to `pu check --entry` and `pu check --caps`; no configuration change. |
| 0.6.1 | `pu serve --host` is `--bind`, and `PU_SERVE_HOST` is `PU_SERVE_BIND`; `serve` in `agents.json` takes `bind`. `GET /api/history` answers `{"messages": [...]}` rather than an array. `GET /api/servers` lists the other servers as `{"name", "port"}`, with no `directory`, `host` or `current`. |
| 0.7.0 | `PU_ASK_DEPTH` no longer exists: a delegated turn gets no `ask_dir`, so it stops at one hop by construction. `ask_dir` takes one `ask` field naming who answers — an agent name from the table's `delegates`, or a directory relative to this one — rather than separate `agent` and `dir` fields; an agent that names no `delegates` may ask nobody. A table that relied on asking an agent by directory is unaffected; one that relied on a tool call spelling out `agent` and `dir` is not. Peer messaging and the task board are removed: `.pu/peers.json`, `PU_PEER_TOKEN`, `send_to_peer`, `/api/jobs`, `/api/peer/message` and `/api/board` no longer exist. Delete the file if a workspace has one; a tool that posted to a peer should use `ask_dir` or read the directory instead. `GET /api/history` no longer reports `from_peer`. |
| 0.7.2 | Same as the entry above. |

## What a session file does across a version

A session is refused rather than guessed at when its `schema_version` is one this build
does not accept: `pu` reports the reason and starts a fresh conversation, and the next
save replaces the file. A file written before the `usage` key existed is not a version
mismatch — absent reads as "nothing was recorded" — so upgrading across that change keeps
the transcript. [runtime.md](runtime.md#single-session-auto-persistence) owns the file's shape.

The one thing a session cannot carry across is an unfinished tool call. A call left
`pending` by an older build keeps refusing `/rewind` until it is answered or the session
is replaced, which is what the 0.5.1 row above is about.
