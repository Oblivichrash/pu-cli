# Release notes

Notable changes, newest first. This file is a summary; a change's reasoning is in
the commit that made it, and its current behaviour is in the relevant document
(see [Where Things Live](CONTRIBUTING.md#where-things-live)).

## Unreleased

### Added

- **The oldest tool-call pairs are retired from the transcript.** Scaffolding was the one pool
  still unbounded: every `execute_bash` the conversation had ever made was re-sent on every
  turn, `tool_calls[].arguments` included, long after the result had been replaced and the
  question had moved on. `RetireOldToolCalls` drops whole pairs — the assistant's call together
  with the receipt that answered it — oldest first, keeping the newest ten. It never splits a
  pair, because a `tool` message whose `tool_call_id` names no call is rejected by every
  provider, and a call with no receipt leaves the model waiting for a result that never comes.
  It runs after the cursor and tombstone tiers, since those are expressed in terms of a call
  that is still there.

### Changed

- **Three names that described the wrong thing are gone.** None of them changes what the
  program does; each one named a file or a module after something it was not.
  - `src/exec/run.cpp` became `src/core/process.cpp` in namespace `pu::core`. It runs one
    child process and reads its output; nothing about it is an executor, and the collision
    with `src/runtime/executor.cpp` — the tool-call loop, which is — made both names
    unreadable.
  - `src/app/serve*` became `src/serve/` in namespace `pu::serve::detail`. Six of the ten
    files in `src/app/` were the HTTP server, and they declared `pu::cli`, so neither the
    directory nor the namespace said what they were.
  - `llm/probe.*` became `llm/field_capture.*`, and `ProbeReport` / `ProbeStream` /
    `FormatProbeReport` became `FieldCapture` / `CaptureFields` / `FormatFieldCapture`. It
    collects the field names a gateway actually sent; it does not probe a gateway, which is
    what `pu probe` does and what the name made people look for.

- **Four interfaces that reported one fact through two channels now report it through one.**
  None of these changes what the program does. Each replaced a parameter whose only job was to
  be written into, or a literal whose meaning was its position, with a value that says what it
  is — so a caller cannot pass the wrong thing without the compiler saying so.
  - `runtime::Turn` is a public aggregate instead of seven numbered arguments, and a provider
    call's fate is `ProviderOutcome` (`kReplied` / `kFailed` / `kStopped` / `kAbandoned`)
    rather than a cross-round `bool abandoned` the loop had to keep alive between rounds.
  - `ExitCode` is an `enum class` in both `app/cli.hpp` and `tools_cli/exit_code.hpp`, which
    exposed two functions that had been declared `int` while returning an exit status:
    `RunProbe` and `RunSse` return `ExitCode` now, and only the two `main`s convert.
  - `text::Join` / `text::JoinQuoted` in `core/text.hpp` are the one way this codebase joins a
    list. Nine hand-written separator loops became calls to it; the empty-range result is a
    property of the helper rather than of each caller's accumulator.
  - `ExecutionResult::was_command` replaces `ProcessInput`'s `bool& is_command`. It sits beside
    `has_error` and `was_streamed`, which the same callers already read from the result;
    `ExitCodeForAsk` lost its second parameter with it.
- **`CommandRouter::Route` answers with a value, and an unknown command is now a failure.**
  It returned `bool` and wrote the text through a `std::string&`, so it could say "not a
  command" and "a command that failed" but not which. `CommandReply` carries `text`, `routed`
  and `ok` separately, and handlers return it instead of writing into a parameter they were
  given. The third fact exposes a real difference in chat: `/unknown` used to come back as
  handled-and-empty, which the front end guessed was an unknown command by testing whether the
  error text was empty. It is now routed-and-failed with `Unknown command: /unknown` as its own
  text, and the guess is gone. `pu ask "/unknown"` still exits 0: a line the router answered
  is not a provider failure, and the exit status names the interruption rather than the
  command. A non-zero status for a failed command would be a separate decision.

### Fixed

- **The exit-status cases no longer read whatever `.pu/` the test process happened to sit in.**
  `Runtime::Initialize` resolves `.pu/agents.json` against the current directory before it asks
  `FindConfigPath`, so a runtime built outside its harness's workspace picked up the repository
  root's config — which names a different set of agents than the harness promises. The working
  directory is the isolation the runtime actually honours; `HOME` alone is not enough.

## 0.7.0

### Changed

- **`ask_dir` names who answers in one field, `ask`.** It used to take `agent` and `dir`
  separately, and every caller had to know which of the two a question wanted — a pair that
  could also contradict each other, since a named agent was only checked against `delegates`
  after the directory had already been resolved. One field removes the choice: a name the
  agent may reach is an agent, and anything else is a directory. The narrower reading wins on
  a name that is both, so a table that lists an agent and a directory under the same word
  still asks the agent, which is what it wrote down. A stored call that used the old pair is
  still drawn as it was.

- **`pu ask` no longer remembers anything.** It used to read `.pu/session.json` before the turn
  and write it after, which made the same prompt mean two different things depending on what
  had been typed earlier in that directory — and made the command unusable as a way to ask a
  question whose whole context the caller supplies. A conversation that is kept is what `chat`
  and `serve` are for. `ask` now sends the prompt and nothing else, and leaves the file alone.

- **`ask_dir` is drawn as a subtask block rather than a tool call.** It arrives over the wire
  as a tool call and still is one, but what it holds is a whole conversation elsewhere: drawn
  as a step, the name or directory asked sat inside a collapsed block's argument JSON, and the
  answer was easy to read as work this turn did. The block names the agent or directory and the
  question in its header, repeats them with the answer in its body, and folds like any other
  step. No protocol change accompanies it — the same `tool_call` record is read differently.

### Added

- **`-C DIR` runs any command as if started in that directory.** A `.pu/` directory is a whole
  world: its own agents table, its own session, its own sandbox root. Nothing tied those three
  to one place before — `--config` moved the table alone, which is how a run could read one
  tree's configuration and write another tree's history. `-C` moves the root, and the rest
  follows from it, so a project with several directories configures a model per directory
  without the directories learning about each other.

- **`ask_dir` lets an agent ask another directory, and get back only its answer.** The turn
  runs there in full — reading files, running commands, paging output — and none of that
  reaches the asking conversation, which grows by one answer rather than by everything it took
  to reach one. It is worth doing only when that directory can find things out for itself,
  which is why it runs a whole runtime rather than sending a prompt to a model. The other
  directory's session is neither read nor written, and a relative path may not climb out of
  the asking directory.

- **An agent can be a directory, and one agent can hand a turn to another.** A `.pu/agents/<name>/`
  holds that agent's own table and, optionally, a `persona.md` whose text becomes its system
  prompt — a document rather than a string, because a character worth describing takes more
  than a line. `ask_dir` takes one `ask` field naming who answers, so the same question can be
  put to a cheap reader and to a careful reviewer without a directory for each; `delegates`
  names the agents an agent may reach and is the whole of what it may reach. `tools` narrows
  which builtins an agent has, and absent means every one, so a reviewer meant to be read-only
  has to say so. A delegated turn gets no `ask_dir` of its own, so a chain stops at one hop,
  and its cost comes back to the turn that asked.

- **`pu-sse` is a standalone tool, installed beside `pu`.** Reading a `text/event-stream`
  needed an endpoint and nothing else, but the only way to reach the reader was through the
  `pu` binary, which links a session, a pager and a tool box none of that work uses. The
  reader now lives in a library of its own and both entry points call it, so a script that
  only watches a feed runs without a workspace or a configuration file, and `pu sse` and
  `pu-sse` cannot disagree about what an event looks like. `cmake --install` places both, so
  one package still installs everything.

- **`pu-probe` is a standalone tool, installed beside `pu`.** A probe asks what a gateway
  sends, which is a question about a wire format and not about a conversation: the subcommand
  could only be reached through a binary that links a session, a transcript and a pager.
  The capture now lives beside the reader in the layer with no session of its own, and both
  `pu probe` and `pu-probe` call it, so the field names one reports are the field names the
  other reports. The tool reads `agents.json` to find a gateway, because a host and a key are
  the one thing a probe cannot invent; it opens no session and keeps nothing between runs.

- **`execute_bash` answers with one page of a command's output instead of all of it.** The
  output is kept in a store that belongs to the session, `at` says where to read (`head`,
  `tail`, or a cursor taken from `page.earlier`/`page.later`), and a receipt carries `page`,
  `buffer` and `extent`: where the page sits in the command's whole output, which run produced
  it, and how much of that run survived the byte budget. A cursor names its own run and the
  store that wrote it, so one left by an earlier process reads nothing rather than a namesake;
  a page of a command that has since been run again is sent as a receipt naming the newer run,
  which is what keeps paging one log ten times down to one page. `paging` in `agents.json`
  sets the budget — see [docs/paging.md](docs/paging.md).

### Removed

- **Breaking** — **Peer messaging and the task board are gone.** Two `pu serve` instances could
  declare each other in `.pu/peers.json`, exchange a shared token, and hand work across as a
  job on the receiver's board: `send_to_peer`, `peers.json`, `PU_PEER_TOKEN`, the `/api/jobs`
  verbs, `/api/peer/message`, `/api/board`, and the board panel in the page. The one thing it
  offered that nothing else does is delegating across two machines, and the part worth having —
  "let another directory answer, bring back only the answer" — is what `ask_dir` now does
  inside one process: it runs the turn where the files are and returns the conclusion alone,
  with no second server, no token, and no state to reconcile. A workspace that carries a
  `peers.json` can delete it; nothing reads the file and no tool answers to it. A session whose
  turns arrived from a peer keeps its messages as ordinary user turns, so no file has to be
  rewritten — but `/api/history` no longer reports `from_peer`, and a cancelled turn is now
  rewound like any other.

### Fixed

- **An MCP server on Windows no longer leaks a little on every start.** The transport hands
  its reader thread a small record of the handle to poll and the flag to clear, and freed
  that record only when the thread failed to start — so a server that ran leaked one on each
  start. The transport now owns the record and frees it when it stops the reader.

### Changed

- **`pu sse --help` and `pu probe --help` say how the two differ.** Both reach a provider and
  both can show what it sent, so a reader working out which to run had to find the difference
  in the documents. The page now says it: `probe` reads `agents.json` for a gateway that is
  already configured, and `sse` takes a URL and headers and reads no configuration at all.
  Nothing about either command changed.

- **Breaking** — **The session API reports `gateway` where it reported `backend_type`.** The
  rest of the API, `agents.json` and the code all call the scheme a gateway, and the one
  field that used the older word was the field a page reads first. A client that read
  `backend_type` reads `gateway`; `backend_model` and `backend_host` are unchanged.

- **Breaking** — **`/backend <type> <model> [host]` no longer takes an API key.** A key
  typed at the chat prompt is written into the scrollback and into whatever records that
  terminal, so the command now refuses a fourth argument instead of accepting one. An
  endpoint that needs a key is named in `agents.json`, which is where every keyed endpoint
  was already configured and where the key can come from the environment. `/backend
  <agent>` and `/backend <type> <model> [host]` are unchanged.

- **`pu probe` is a developer tool and no longer appears in `pu --help`.** It captures a
  provider's raw stream for someone writing a `ResponseShape`, which is a question about a
  gateway rather than about an `agents.json`. It is unchanged and still documented in
  [README.md](README.md#core-commands); only the top-level listing and its `--command` hint
  leave it out. `pu models` remains the answer to what a gateway offers.

- **Breaking** — **`pu check --caps` and `pu check --entry` are `pu models --detail caps`
  and `pu models --entry`.** Both read the listing `pu models` already prints, and `pu
  check` now answers only its own question: what an `agents.json` resolves to, without
  contacting a provider. `pu models` also takes `--gateway` and `--model`, which it was the
  only listing command without. No configuration change.

- **Breaking** — **`session.json` is schema 9: a tool receipt carries `origin`.** The
  question it answered, the run that answered it and that run's rank are now written with
  the receipt instead of being parsed out of its text on every turn. A file written by an
  older build names schema 8, so it is moved to `session.json.bak` and a fresh conversation
  starts, with the reason printed. Nothing else about a session changed: a rewind, a reload
  and a page read after a newer run all behave as they did.

### Removed

- **Breaking** — **`GET /api/workspaces` and `POST /api/clear` are gone.** Neither had a
  caller: the page names another server from `/api/servers`, which lists what this machine is
  actually serving rather than every sibling directory that carries a `.pu/agents.json`, and it
  clears a conversation by sending the new one. A second way to say either thing was a surface
  to keep in step with no reader to keep it honest. `pu /clear` still clears a conversation in
  the terminal, and `/api/rewind` still steps back a turn over HTTP.

### Fixed

- **`pu check` no longer calls `paging` a field this build ignores.** The check keeps its own
  list of the keys a file may hold, and `paging` was missing from it although the parser reads
  it, so the command advised deleting a budget that was in force. The block's own keys are now
  checked too: `page_line` for `page_lines` is named instead of being silently ignored.
- **A test run no longer leaves roughly a hundred directories behind in the temp directory.**
  Both server fixtures point `PU_HOME` at a directory of their own, and both kept that path
  only for as long as the call that made it: the serve fixture held it inside the environment
  block that restored it, and the peer fixture held it in a local. Neither destructor could
  name the directory, so neither removed it, and every case left one behind. The path is now
  a member and the slot each destructor already used removes it. The two logging cases that
  named a fixed directory now use one of their own, so cases running in parallel no longer
  write to the same one. `ctest` names the temp directory too, so a fixture that is ever
  missed again leaves its files in the build tree rather than outside it.

## 0.6.0

### Breaking

- **`--host` is `--bind`, and `PU_SERVE_HOST` is `PU_SERVE_BIND`.** `host` already
  names a backend's upstream address in this program, and `0.0.0.0` is a legal bind and never a
  destination; the two are not the same question. A script or unit file that passes `--host`
  stops with an unrecognised option rather than binding somewhere unexpected, and one that sets
  `PU_SERVE_HOST` falls back to `127.0.0.1`. `serve` in `agents.json` takes `bind` for the same
  reason.
- **`GET /api/history` answers an object, not an array.** Every route now answers
  one envelope with the payload under the key its row names, so the listing is
  `{"messages": [...], "success": true}`. A client that read the body as an array has to read
  `.messages`.
- **`GET /api/servers` describes other servers, not this one.** An entry is
  `{"name", "port"}`: a label rather than a filesystem path, and no `current` mark, because the
  server that answered is the page's own and is no longer in its own list. A client that read
  `directory`, `host` or `current` has to take the name from `/api/session` instead.

### Added

- **The context bar has a figure to draw against.** `backend.context_window` in `agents.json`
  names how much context the model holds, and `GET /api/session` answers it so the page can fill
  its bar instead of leaving the count alone. Advisory throughout: nothing trims a prompt against
  it and no turn is refused for outgrowing it, so a wrong figure costs a misleading bar rather
  than a broken turn. It is stated in the file rather than read from the gateway because it is
  asked for while the session's lock is held, and a catalogue read there would stand between the
  page and every other request for as long as the gateway took — `pu check --caps` prints what
  each gateway reports, which is where a number to write comes from. `0` means nobody named one
  and the bar stays empty: DeepSeek reports 1M, CodeBuddy claims 1M and answers past 250K as
  though it fit, and NIM reports nothing at all.

### Fixed

- **A page opened over the network no longer loses its own name.** The header learned which
  workspace it was showing from `GET /api/servers`, and that route answers on the machine the
  server runs on: a page reached through `--bind` got `403`, and `refreshServers` returned on the
  failure without drawing, so `#server-current` stayed at its placeholder for the whole visit. The
  page's own name now comes from `/api/session`, which every caller may read, and the list is a
  separate fact that can be empty. A refused list still draws the header, with a tooltip saying
  why the switcher is unavailable.
- **A server whose process exited no longer lingers in the list when its pid is reused.** The
  registry decided liveness from the pid alone, and a pid is unique only among the processes
  running now: once its owner exits the number is handed to the next program, so the record read
  as alive and the page offered a switch to a server nobody could reach. Each record now carries
  the process's start time and is kept only while both match.
- **A reply no longer stops short of its last words.** A redraw waits for the next animation frame
  so a fast stream costs one paint rather than one per chunk, and ending the turn threw the bubble
  away before that frame ran: the queued text was dropped and the answer stayed on screen as a
  prefix of what the store held, which is why reloading showed the rest. Ending a turn now draws
  what is owed first. A tab in the background has the same hole from the other side: a hidden tab
  is not painted, so a redraw it is holding waits for the reader to come back, and the reply stops
  where they left it until then. The page now draws its pending frame as it goes out of sight.

### Changed

- **Who may read a route is a column of the route table.** `Audience` names `anyone`, `loopback`
  or `peer`, `Route` has no default for it, and the dispatcher holds no path names of its own — so
  a new route answers the question by compiling, and one whose answer changed fails a case that
  reads the table. `/api/workspaces` is now `loopback`: it answers this machine's directories and
  was previously readable by any caller that reached the port. `handle` signatures take a `Call`
  that carries the runtime, the board, the runner, and the three facts only the connection knows
  (the caller's address, the peer its token speaks for, and the port this server answers on).
- **One envelope for every answer.** `Route::key` names the member a payload is filed under, and
  the `Envelope` enum is gone, so a listing cannot invent a fifth shape. The shape `/api/history`
  answers is in **Breaking** above, since a client has to change for it.
- **The three audience refusals are three statuses.** `403` is a route that does not answer this
  address, `401` (with `WWW-Authenticate: Bearer`) is a peer route with no token or the wrong one,
  and `404` is a peer route on a server with no topology. A refused peer token was `400`.
- **The start-up banner names the port, not a URL built from the bind address.**
  `http://0.0.0.0:9000` was printed for a server bound to every interface, and that address reaches
  the reader's own loopback rather than the server. The port is the one fact the server knows; the
  address is whoever opened it.
- **`GET /api/servers` lists the other servers and leaves this one out.** The server that answered
  is the page's own, so a switch to it would be a control that does nothing. Filtering is by
  process rather than by directory, so two servers on one workspace do not offer each other. A
  sibling's address is that port on the host the page already reached, since a registry `bind` is
  not a destination. The entry's shape is in **Breaking** above.

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
