# Release notes

Notable changes, newest first. This file is a summary; a change's reasoning is in
the commit that made it, and its current behaviour is in the relevant document
(see [Where Things Live](CONTRIBUTING.md#where-things-live)).

## Unreleased

### Changed

- `pu --help` names each command with the one line that says what it answers. The list is
  built from the same table the `--command` flag is described from, so a command cannot be
  added to one and forgotten in the other.
- **Six of the seven project rules are gone.** The cases that read Markdown and source text —
  the SPDX line, the test file list, every document reachable from outside `docs/`, every
  relative link and anchor, every closed inline code span, and the repeated `###` heading in a
  CHANGELOG section — are removed with nothing replacing them, so each of those rules is a
  reviewer's again. The page rule stays, because it decides how the page runs rather than how
  the text reads.

## 0.6.0

### Added

- **The page names the servers on the machine, not the ports.** Each `pu serve` writes itself
  into `$HOME/.pu/serve.json` while it runs — the directory it serves, the address it answers
  on, and its pid — and removes the line when it stops, so a server started on port `0` is
  still listable and a crash costs a stale line that the next reader drops. The read and the
  write take a lock file beside it, so two servers starting at once cannot erase each other.
  `GET /api/servers` answers that list with `current` marking the server that answered; it
  is loopback-only, because its entries are filesystem paths. The header shows the
  workspace this page is talking to, with the others behind it once there is more than one, so
  remembering which port belongs to which directory is no longer the reader's job. Choosing one
  moves the page to it and rebuilds: the socket reconnects, the transcript and the board are
  read again, and the server left behind keeps running its turn, which is why the switcher is
  held while a reply is in flight.
- **The board has a panel on the page.** The sidebar gains a board that lists every open job with
  the peer it came from, its intent, its state, and — when the deadline has passed — that it is
  late. A blocked job is answered from there, since a question the page shows and cannot answer
  is a question a reader has to go to another terminal for. The panel reads `/api/board` on
  its own clock, because a peer moving a job is not something the session's socket reports, and
  draws nothing at all when the board is empty. That read is a route of its own: `/api/jobs`
  records the reader as heard from and takes its name from the token, and a page has neither, so
  the page route answers the work still owed and the few jobs that settled most recently, without
  the peers, and is refused to anything not on loopback.
- **A person can ask what is happening, and take a job back.** `POST /api/jobs/chase` records a
  note the holder reads in its digest and counts the ask on the job, leaving the job exactly
  where it is: a job that is slow is not a job that moved. `POST /api/jobs/drop` closes a job the
  person no longer wants as `cancelled`, which is a state only a person can bring about — a peer
  that could say it would be able to end a job it was handed without doing it. Both are drawn on
  the board panel next to the answer box, and both read by the holder from the board rather than
  from a message, so a cancellation that arrives after the work finished changes nothing.
- **A person can answer a blocked job.** `POST /api/jobs/answer` takes the job and the answer,
  records the answer as the job's `note`, clears the `question`, and returns the job to
  `working` for the peer holding it. It is a verb of its own rather than a `PATCH`, because a
  person has no name to present as `from` and borrowing a peer's would make the audit trail say
  a peer answered its own question. `blocked` was a state with no way out; this is the way out.
- **The board is in the model's context, refreshed every round.** `Runtime::SetContextDigest`
  hands the executor a function rather than a string, and `BuildStaticSystemContext` calls it on
  every round of every turn, so a job claimed mid-turn reads as claimed on the next round
  without the model asking. `Board::Digest(self_name)` lists the jobs this server holds — with
  the peer they came from, the intent, the state, any question it is being asked, and whether
  the deadline has passed — plus the jobs it handed out that have since closed, with their
  results. So a peer learns it was handed work, and learns its own outcome, without a message
  and without a poll. An empty board adds nothing to the request.
- **A hand-off is a job before it is a message.** `send_to_peer` posts the job on the receiving
  server's board, then sends the message that names it; the receiver claims the job before it
  queues the turn, and closes it with `done` and the answer — or `failed` and the error — when
  the turn ends. A board that refuses the job means nothing is sent at all, so a hand-off that
  the receiver could not accept never arrives as a turn. The tool takes an `intent` naming the
  work and an optional `due` duration, and answers with the job, the turn and the model.
- **A task board, so work handed to a peer outlives the message that asked for it.** A job is a
  shared object with an owner and a state — `open`, `claimed`, `working`, `blocked`, `done`,
  `failed` — and only the peer it is offered to may move it, so two peers can neither work the
  same job nor lose track of who holds it. `blocked` requires a question, because a job that
  stops without saying what it waits for is indistinguishable from one that was dropped.
  Declining is not a state: it records `declined_by`, counts up `declines`, and puts the job
  back to `open` for whoever is scheduling to offer elsewhere.
- **A peer is heard from by reading, and the reader decides what that means.** There is no
  heartbeat call: every board request carries `Authorization: Bearer`, so the peer a token
  belongs to is recorded as heard from at the moment of the read, and the same answer carries
  what the board holds. `online` is derived from the age of `last_seen` rather than claimed, so
  a wedged peer cannot report itself well and a peer with a wrong clock cannot look healthy.
  The pull is made by the turn loop rather than by a timer, so "still reading" and "still
  working" are the same statement.
- **Two clocks on the board, both derived, neither stored.** `due` is given as a relative
  duration when a job is offered and resolved to `due_at`, because a model can say how long it
  expects to need but not reliably what time it is; `overdue` and `idle_seconds` are computed on
  every read, so a job goes overdue by the passage of time alone. Overrunning and blocking are
  visible on the board rather than sent as messages — the only replies they invite are
  reassign or intervene, and both are the human's to make.
- **What a conversation has spent, summed and drawn.** `/api/session` carries `usage_total`
  (turns, rounds, prompt/completion/cached tokens, `priced_turns`, and a price only when every
  priced turn agreed on its unit). The page draws it under the context bar, and each reply
  carries a line with its own cost — including the cached share, which the file recorded but
  nothing displayed.
- **One settings table instead of a field per setting.** `thinking`, `temperature`, `max_tokens`
  and `tool_budget` are rows in `pu/session/settings.hpp`; `SessionSpec` keeps the session's
  overrides as a map, and `/api/settings` reports every row with the value in force and whether
  it came from this session or the agent. `/api/thinking` is gone: the page draws one row per
  setting the server offers, so a new row appears without a page change.
- A page is handed the frames of the turn that ran while it was not open, rather than
  needing a reload to see it: `SessionRunner` keeps a record per turn (the last eight) and
  gives the turn in flight together with the last finished one to whoever attaches.
- A user turn carries `from_peer` when it arrived from another server. The page names the
  peer where it would otherwise say "You", the field is in `/api/history`, and the model
  never reads it.
- `/api/rewind` refuses a turn a peer asked, since it is not this page's to take back.
- What each turn cost is now stored with the session and served by `/api/history`: prompt,
  completion and cached tokens, the price where the product reports one, how many provider
  calls the turn took, and the model and gateway that answered. The key is absent from older
  files and read as "nothing was recorded", so the schema version is unchanged.
- **A price is a product's answer, not a protocol's.** `Usage.credit` (a bare double that
  defaulted to zero) became `Usage.cost` — an amount and the unit the product named — and the
  field it arrives in is declared on the gateway that prices calls. A product that reports no
  price now leaves the cost absent instead of zero, so a summed total no longer silently
  undercounts. Only CodeBuddy reports one today, in `credits`.
- `pu sse URL [--header H] [--post] [--body B] [--format text|json|raw]` reads any
  `text/event-stream` and prints what arrives. A header repeats, `--format json` is one object
  per line for a pipe, and `--format raw` is the bytes as sent. It is a general client: the URL,
  the verb and the headers are the caller's, and nothing is read from `agents.json`.
- `pu check --caps` prints the columns that decide between two models that both work: what
  each accepts, whether an effort is mandatory (the flag that predicts a refused request),
  the context a model takes, and the price where the gateway reports one.
- `pu ask --show-usage` prints what the turn cost — prompt, completion and cached tokens, the
  price where the product reports one, and how many provider calls it took — on stderr. stdout
  stays the answer.
- `pu agents [--json]` lists the configured agents and marks the one the session names. The
  chat's `/agents`, `GET /api/agents` and this now build their answer in one place, so the
  three cannot describe the same configuration differently.
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

- **Breaking** — **One flag chooses `pu sse`'s shape, and `pu models` only lists.** `pu sse` took `--json`,
  `--quiet` and `--raw` as three independent switches, two of which could be given together and
  the third of which was not a shape at all; it takes `--format text|json|raw` instead, and
  `--quiet` still narrows the text shape. `pu models` listed the models and also printed one
  model's `agents.json` entry (`--entry`) or its capabilities and price (`--caps`), which are
  questions about a model rather than about the list; both flags moved to `pu check`, the command
  that answers what a configuration resolves to without contacting a provider. A listing is
  read, a model is checked.
- **The page is served from its directory, not from a list of names.** The server answered
  `/app.js` because `app.js` appeared in a hardcoded array, so a module added to `web/` and its
  `<script>` tag was still a `404` until someone remembered a second place. The route now asks
  the directory, and a name that is a path rather than a file name never reaches it. A new
  module is a file and a tag.
- **The rules a machine can decide are checked by the suite.** `tests/unit/test_project_rules.cpp`
  fails on a missing SPDX line, a test file the build does not name, a document nothing links to,
  a relative link or anchor that resolves to nothing, a `web/*.js` that is shipped but not loaded
  (or loaded but not shipped, or listed after `app.js`), and a repeated `###` heading in a
  CHANGELOG section. Each rule was previously a line in CONTRIBUTING that a reviewer had to
  remember.

- **The CLI has exit codes.** `0` did what it
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
- A peer could not move a job to `cancelled` already, because `ParseJobState` never yields
  that state, but `Board::Move` said so only by omission and GCC reported the gap as
  `-Wswitch`. The state is now refused by name in the same place, so the rule the board
  documents is the rule the code states.

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
- `pu models --entry <id>` prints an `agents.json` entry ready to paste. It answers about a
  model rather than about the list, so from 0.6.0 it is `pu check --entry`.
- Token usage and the context a turn ran against are carried to the Web UI.
