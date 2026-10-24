# Web UI and API

`pu serve` starts a local web UI on top of the same single-session runtime. Open
`http://127.0.0.1:8080` in a browser and chat with the active agent:

```bash
./build/pu serve                       # listen on 127.0.0.1:8080
./build/pu serve --bind 0.0.0.0 --port 9000
```

`--bind` names the interface the listener sits on, not an address to open. `0.0.0.0` is a
legal bind and never a destination a browser can dial, so the start-up banner says which
port it took and leaves the address to the reader: the page names this server by the
address you reached it on.

A server serves the directory it was started in and stays there: nothing moves it to
another one. Each directory is a session with its own configuration, conversation and
tools, so serving several of them at once means one server per directory — which is
also what keeps them from sharing a conversation or a turn. Since that needs a port per
directory, the workspace can name its own (`--bind`/`--port` first, then
`PU_SERVE_BIND`/`PU_SERVE_PORT`, then the file):

```json
"serve": { "bind": "127.0.0.1", "port": 8087 }
```

A port is what a reader has to remember to tell two servers apart, and the page is what
has to do it for them. Each server writes itself into `$HOME/.pu/serve.json` while it
runs — directory, bind, port, pid and the process's start time — and deletes the line on the
way out, so the page can list the machine's servers and name each one by its workspace. A
record whose process has stopped is dropped the next time the file is read, which is what
makes a crash cost a stale line rather than a ghost entry. The start time is there because a
pid outlives the program that held it: once its owner exits the number is handed to the next
program, so a reader that asked only whether the pid was alive would get a true answer about
something else and list a server nobody can reach. Two servers starting together would
otherwise each read the list before the other had written it and the second write would
drop the first, so the read and the write happen under `serve.json.lock`, held for the few
milliseconds the update takes; the temp file each writer renames is named after its own
process, so a writer that gave up waiting on the lock cannot publish over one that finished.
The file is a record of what is running, not a setting: `serve` in `agents.json` is still the
port a workspace asks for, and the registry is what actually answers.

> **Warning** — the server has no authentication and no origin checks, so
> anyone who can reach the port can read the session history and run the active
> agent's tools. Keep the default loopback bind unless the port is protected by
> other means.

Each message carries an **edit** link that steps back to before that turn; sending
the text again replaces it. See `/rewind` in the [README](../README.md#core-commands).

The front-end lives in `web/` as plain scripts with no build step, so the load order in
`index.html` is the dependency order and each file has one job — see
[ARCHITECTURE](ARCHITECTURE.md#browser-modules). History is folded into one bubble per
turn, with each tool result keeping the output the stream showed. Under the composer it shows
the context the last reply ran against, from the counts the provider returned, and how much of
the model's window that is when the backend states one. A backend that states none leaves the
bar empty and the count alone, because a guess would read like a fact; `context_window` in
`agents.json` supplies a figure the gateway does not.

## WebSocket

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
{"type":"usage","payload":{"prompt_tokens":48213,"completion_tokens":312,"cached_tokens":41000,"rounds":2,"model":"gpt-4o-mini-2024-07-18","gateway":"","total":{"turns":7,"rounds":11,"prompt_tokens":290144,"completion_tokens":2210,"cached_tokens":241000,"priced_turns":0}}}
{"type":"busy","payload":{"text":"This session is already open in another page."}}
{"type":"done","payload":{"model":"gpt-4o-mini-2024-07-18"}}
{"type":"error","payload":{"text":"error description"}}
```

`usage` carries what the turn that just ended cost; `prompt_tokens` is the context
that turn ran against, so it grows with the conversation and is what a page can show
against a model's window. It arrives before `done`, and a provider that reports no
counts sends none. `rounds` counts the provider calls the turn made, since one reply
that called three tools spent two rounds and the counts are both their sum. `cost`
appears only where the gateway priced the call, under the gateway's own unit.

The frame describes a turn with the same object `/api/history` replays beside the
finished message, so a page parses one shape whether a turn is live or reloaded. Two
fields differ: `gateway` is empty on the frame and names the gateway on a replayed
turn, and `total` appears only on the frame. `total` is the same object
`/api/session` answers as `usage_total`, sent with the turn because it is already
known, so a page that has just watched a turn does not have to ask again to draw it.

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

## REST endpoints

Every answer is one object carrying `success`, with the payload under the key the route names:
`{"messages": [...], "success": true}` for a listing, and the handler's own members merged
in for everything else. The key is part of the route rather than something each handler
decides, so two listings cannot drift into two shapes.

**Who may read a route is a column of this table, not a comparison of paths in the
dispatcher.** A row that does not state its audience does not compile. `anyone` means the
bind address is the control, which is why the bind is the setting that matters;
`loopback` means the answer is a fact about this machine and the route refuses a caller
from anywhere else with `403`; `peer` means the shared token, and a caller without it is
refused with `401`.

| Method | Path | Audience | Description |
| :----- | :--- | :------- | :---------- |
| `GET` | `/api/session` | anyone | Current session info (agent, backend type/model), `context_window` (the model's window, `0` when nobody states one), the settings in force, `usage_total`: what the conversation has spent, and `server`: the `name` and `port` this server answers by |
| `GET` | `/api/history` | anyone | `messages`: the full conversation (user, assistant and tool messages); every tool call carries `tool_call_status`, every result the parsed `output`/`error` the stream showed, and a user turn that arrived from a peer carries `from_peer` |
| `GET` | `/api/agents` | anyone | `agents`: every available agent with its description |
| `POST` | `/api/agent/switch` | anyone | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/workspaces` | loopback | `workspaces`: every sibling directory this machine could serve (each containing `.pu/agents.json`), with its name and its path |
| `GET` | `/api/servers` | loopback | `servers`: the **other** servers on this machine, each with a `name` and a `port`. The one that answered is not in the list |
| `POST` | `/api/clear` | anyone | Clear the conversation history |
| `POST` | `/api/rewind` | anyone | Step back to before a turn (`{"turn":n}`); the next message replaces it. Refused for a turn a peer asked |
| `GET` | `/api/settings` | anyone | Every setting this build knows, with the value in force and whether it came from this session or the agent |
| `POST` | `/api/settings` | anyone | Set one (`{"name":"tool_budget","value":25}`); a missing or `null` `value` clears it |
| `POST` | `/api/settings/reset` | anyone | Drop every override and go back to the agent's configuration |
| `GET` | `/api/board` | loopback | The board as the page draws it: the work still owed, the last few that settled, and the trail. No peers, no token |
| `GET` | `/api/jobs` | peer | The board: every open job, every peer with its derived `online`, and the trail of what was settled. Reading it is how a peer is heard from |
| `POST` | `/api/jobs` | peer | Offer a job to a peer (`{"to":"B","intent":"...","due":"30m"}`, `due` optional) |
| `PATCH` | `/api/jobs` | peer | Move one (`{"job":"...","from":"B","state":"working"}`, plus `question`, `result`, `note`) |
| `POST` | `/api/jobs/answer` | peer | Answer a blocked job's question (`{"job":"...","answer":"..."}`); the person's verb |
| `POST` | `/api/jobs/chase` | peer | Ask a job that has not closed what is happening (`{"job":"...","note":"..."}`); the job stays where it is |
| `POST` | `/api/jobs/drop` | peer | Take a job back before it finishes (`{"job":"...","note":"..."}`); it closes as `cancelled` |
| `POST` | `/api/peer/message` | peer | Admit a message from a configured peer as a new turn (`{"from":"A","message":"...","job":"...","hop":0}`); answers `202` rather than a reply |


A request that fails is answered with the status that names whose it is: `400` for one the
caller can fix — a body that is not JSON, a value that is missing or unknown, a state that
cannot serve it, such as a step back while a tool call is pending — `404` where a name or a
route does not exist (including a peer route on a server with no `peers.json`), `401` for a
peer route with no token or the wrong one, `403` for a route whose audience excludes the
caller's address, and `500` where the operation failed. A refusal carries `success: false`
and the reason in `error`.

The three audience refusals are three different problems and are not interchangeable. `403`
says the caller was identified and the route does not answer it from there. `401` says the
caller never said who it was, and carries `WWW-Authenticate: Bearer`. A `404` from a peer
route says the server has no topology at all, which is a fact about this build rather than
about the request.

## The servers

The page names the server it is attached to, and lists the others. The two facts have two
homes, which is what keeps either one from being blank when the other is unavailable.

**Its own name comes from the server that answered.** `GET /api/session` carries
`server: {"name", "port"}`, and the name is the last two segments of the directory that
server serves — `a/pu-cli` rather than `/home/a/pu-cli`. That is the label the header draws.

**The others come from `GET /api/servers`**, which answers `{"servers":[{"name","port"}]}`,
one entry per other running `pu serve` on this machine, read from the registry the servers
write themselves into. The server that answered is not in its own list: it is the page's own,
and a switch to where the reader already is is a control that does nothing. Filtering by
process rather than by directory is what makes that true when two servers serve the same
workspace, which is legal.

Entries carry a label rather than a path, but the route is still loopback only. What it
answers is the shape of this machine — which projects it serves, and on which ports — and a
list of that is a map of somebody's disk. So a server bound to another address still serves
the page, the rest of the API and the board, but not the list. A page that reached one over
the network gets `403`, keeps its own name from `/api/session`, and says in the header why it
has nothing to offer.

A sibling's address is that port on the host the page already reached, because the registry's
`bind` is where a listener sits and not a destination: `0.0.0.0` is a legal bind and never a
legal address to dial, and a server behind a proxy would name one this page cannot reach.

The page is one server per tab by nature — one socket, one transcript — but naming them is
not: the header shows the workspace this page is talking to, and lists the others when there
are any. Choosing one moves the page to it. Switching is deliberately light: the socket is
closed and reopened against the chosen server, the transcript and the board are read again
from it, and the server that was left keeps running the turn it was given. That is why a turn
in progress holds the switcher: nothing here moves a running reply between servers, and
pretending otherwise would be the one case where a reader loses work.

## The board

`GET /api/jobs` answers `{"jobs":[...],"peers":[...],"audit":[...]}`, and a call made with a
peer's token adds `self` for whichever peer that token belongs to. A job carries `id`, `from`, `to`, `intent`, `state`,
`created_at`, `updated_at`, `idle_seconds`, `closed`, and — each only when it has something to say —
`due_at`, `overdue`, `question`, `result`, `note`, `chases`, `declined_by` and `declines`.
A peer carries
`name`, `state`, `last_seen`, `seen_seconds` and `online`.

`overdue` and `online` are read from the clock each time the board is read and are never
stored, so a job goes overdue and a peer goes offline on their own, without anyone having
to write it down. `online` means the peer was heard from within 45 seconds.

`POST /api/jobs` answers with the created job. `PATCH /api/jobs` answers with the updated
job, plus an `outcome` when the move settled it. A move the job does not allow is refused
with `400`: a move by a peer that does not hold it, a move on a job already settled,
`blocked` without a `question`, an unknown `state`, or a `due` that is not a duration. A
job name that is not on the board answers `404`. The board holds at most 32 open jobs.

**Reading is being heard from.** Every board request presents `Authorization: Bearer` with
the shared token, and the peer that token belongs to is recorded as heard from at the moment of
the read. There is no separate heartbeat call: one request both says "still here" and answers
with what the board holds, which is the same round trip. A request without the token is refused
with `403`, because a liveness nobody can prove is not one.

`POST /api/jobs/answer` takes `job` and `answer` and is the person's verb, not a peer's: it is
how a `blocked` job gets unstuck. The answer is recorded as the job's `note`, the `question` is
cleared, and the job returns to `working` for the peer that holds it. It is a separate call
rather than a `PATCH` because a person has no name to present as `from`, and borrowing one
would make the audit trail say a peer answered its own question. A job that is not blocked
answers `400`; a job that is not on the board answers `404`.

`POST /api/jobs/chase` and `POST /api/jobs/drop` are the person's other two verbs, and they are
separate calls for the reason `answer` is: a person has no name to present as `from`. A chase
records the note, clears any `question` (the holder was just asked directly, so the job is
no longer blocked on silence) and counts a `chases`, leaving the state alone, because a job
that is slow is not a job that moved. The holder reads both the count and the note in its
digest. A drop closes the job as `cancelled`, the one state no peer can ask for: a peer that
could say it would be able to end a job it was handed without doing it. Both refuse a job that
has closed (`400`) and a job that is not on the board (`404`).

**The page is shown a board it can draw, and is not being heard from.** `GET /api/board`
answers `{"open":[...],"settled":[...],"audit":[...]}`, needs no token, and is refused with
`403` to any request that does not come from `127.0.0.1` or `::1`. `open` is every job that has
not closed, in the order it was posted; `settled` is the three that closed most recently, newest
first. Which states count as settled is the board's to say, so the page draws a job it is given
and never decides whether one still needs anything; a job carries `closed`, and the page hands
only the open ones their verbs.

A page has no secret to present, and a page polling says nothing about a peer, so the route
records nothing about who is here and answers no `peers` — a liveness answer from a browser
would read as a peer that never leaves. The board panel is its caller: it polls this route,
answers blocked jobs from the sidebar, and takes a job back from it, which is why a peer's
question is answerable without opening another terminal.

The address is the whole of that credential, so a server bound to every interface serves the
panel only on the machine it runs on: reach the page at `127.0.0.1` to see the board, and
expect a page opened over the network to draw no panel at all — the read is refused, and a
panel that cannot read the board draws nothing rather than an empty one. Chat, history and the
rest of the page are unaffected, and the network exposure above is what governs those.

When a peer message names a `job`, the receiver claims it before it queues the turn and
answers with `from`, `turn`, `agent` and `model`; a job that is not on this board, or not this
peer's to claim, is refused and no turn starts. The turn closes the job when it ends — `done`
with the answer, `failed` with the error — so the sender learns the outcome from the board
rather than by waiting on the peer.

The board is also read directly by the model. Every round of every turn carries a digest of
what the board owes this server, refreshed in place, so a peer learns that it was handed a job
and learns its own outcome without a request. See "What the model is told, and when" in
[ARCHITECTURE.md](./ARCHITECTURE.md).
