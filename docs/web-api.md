# Web UI and API

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
the text again replaces it. See `/rewind` in the [README](../README.md#core-commands).

The front-end lives in `web/` as plain scripts with no build step, so the load order in
`index.html` is the dependency order and each file has one job — see
[ARCHITECTURE](ARCHITECTURE.md#browser-modules). History is folded into one bubble per
turn, with each tool result keeping the output the stream showed. Under the composer it shows
the context the last reply ran against, from the counts the provider returned. No
window is shown beside it: none of the backends here reports one, and a guess would
read like a fact.

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

## REST endpoints

| Method | Path | Description |
| :----- | :--- | :---------- |
| `GET` | `/api/session` | Current session info (agent, backend type/model), the settings in force, and `usage_total`: what the conversation has spent |
| `GET` | `/api/history` | Full conversation history (user, assistant and tool messages); every tool call carries `tool_call_status`, every result the parsed `output`/`error` the stream showed, and a user turn that arrived from a peer carries `from_peer` |
| `GET` | `/api/agents` | List all available agents with descriptions |
| `POST` | `/api/agent/switch` | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/workspaces` | List all workspaces (directories containing `.pu/agents.json`) |
| `POST` | `/api/clear` | Clear the conversation history |
| `POST` | `/api/rewind` | Step back to before a turn (`{"turn":n}`); the next message replaces it. Refused for a turn a peer asked |
| `GET` | `/api/settings` | Every setting this build knows, with the value in force and whether it came from this session or the agent |
| `POST` | `/api/settings` | Set one (`{"name":"tool_budget","value":25}`); a missing or `null` `value` clears it |
| `POST` | `/api/settings/reset` | Drop every override and go back to the agent's configuration |
| `POST` | `/api/peer/message` | Admit a message from a configured peer as a new turn (`{"from":"A","message":"...","job":"...","hop":0}`); requires `Authorization: Bearer`, answers `202` rather than a reply |
| `GET` | `/api/jobs` | The board: every open job, every peer with its derived `online`, and the trail of what was settled |
| `POST` | `/api/jobs` | Offer a job to a peer (`{"to":"B","intent":"...","due":"30m"}`, `due` optional) |
| `PATCH` | `/api/jobs` | Move one (`{"job":"...","from":"B","state":"working"}`, plus `question`, `result`, `note`) |
| `POST` | `/api/peers/heartbeat` | Say a peer is alive (`{"peer":"B","state":"working"}`) and read the board back in the same round trip |

A request that fails is answered with the status that names whose it is: `400` for one the
caller can fix — a body that is not JSON, a value that is missing or unknown, a state that
cannot serve it, such as a step back while a tool call is pending — `404` where a name does
not exist, and `500` where the operation failed. A refusal carries `success: false` and the
reason in `error`.

## The board

`GET /api/jobs` answers `{"jobs":[...],"peers":[...],"audit":[...]}`, and a heartbeat adds
`self` for the peer that asked. A job carries `id`, `from`, `to`, `intent`, `state`,
`created_at`, `updated_at`, `idle_seconds`, and — each only when it has something to say —
`due_at`, `overdue`, `question`, `result`, `declined_by` and `declines`. A peer carries
`name`, `state`, `last_seen`, `seen_seconds` and `online`.

`overdue` and `online` are read from the clock each time the board is read and are never
stored, so a job goes overdue and a peer goes offline on their own, without anyone having
to write it down. `online` means the peer was heard from within 45 seconds.

`POST /api/jobs` answers with the created job. `PATCH /api/jobs` answers with the updated
job, plus an `outcome` when the move settled it. A move the job does not allow is refused
with `400`: a move by a peer that does not hold it, a move on a job already settled,
`blocked` without a `question`, an unknown `state`, or a `due` that is not a duration. A
job name that is not on the board answers `404`. The board holds at most 32 open jobs.

`POST /api/peers/heartbeat` requires `peer` and reads back the whole board, so a peer learns
what it holds in the same round trip that says it is alive.

`POST /api/jobs/answer` takes `job` and `answer` and is the person's verb, not a peer's: it is
how a `blocked` job gets unstuck. The answer is recorded as the job's `note`, the `question` is
cleared, and the job returns to `working` for the peer that holds it. It is a separate call
rather than a `PATCH` because a person has no name to present as `from`, and borrowing one
would make the audit trail say a peer answered its own question. A job that is not blocked
answers `400`; a job that is not on the board answers `404`.

When a peer message names a `job`, the receiver claims it before it queues the turn and
answers with `from`, `turn`, `agent` and `model`; a job that is not on this board, or not this
peer's to claim, is refused and no turn starts. The turn closes the job when it ends — `done`
with the answer, `failed` with the error — so the sender learns the outcome from the board
rather than by waiting on the peer.

The board is also read directly by the model. Every round of every turn carries a digest of
what the board owes this server, refreshed in place, so a peer learns that it was handed a job
and learns its own outcome without a request. See "What the model is told, and when" in
[ARCHITECTURE.md](./ARCHITECTURE.md).
