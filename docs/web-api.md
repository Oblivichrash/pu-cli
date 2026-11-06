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
> other means. [security.md](security.md) is where the rest of the trust boundary is
> stated.

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
from anywhere else with `403`.

| Method | Path | Audience | Description |
| :----- | :--- | :------- | :---------- |
| `GET` | `/api/session` | anyone | Current session info (agent, gateway/model), `context_window` (the model's window, `0` when nobody states one), the settings in force, `usage_total`: what the conversation has spent, and `server`: the `name` and `port` this server answers by |
| `GET` | `/api/history` | anyone | `messages`: the full conversation (user, assistant and tool messages); every tool call carries `tool_call_status`, every result the parsed `output`/`error` the stream showed |
| `GET` | `/api/agents` | anyone | `agents`: every available agent with its description |
| `POST` | `/api/agent/switch` | anyone | Switch to a different agent (`{"agent_name":"..."}`) |
| `GET` | `/api/servers` | loopback | `servers`: the **other** servers on this machine, each with a `name` and a `port`. The one that answered is not in the list |
| `POST` | `/api/rewind` | anyone | Step back to before a turn (`{"turn":n}`); the next message replaces it |
| `GET` | `/api/settings` | anyone | Every setting this build knows, with the value in force and whether it came from this session or the agent |
| `POST` | `/api/settings` | anyone | Set one (`{"name":"tool_budget","value":25}`); a missing or `null` `value` clears it |
| `POST` | `/api/settings/reset` | anyone | Drop every override and go back to the agent's configuration |
A request that fails is answered with the status that names whose it is: `400` for one the
caller can fix — a body that is not JSON, a value that is missing or unknown, a state that
cannot serve it, such as a step back while a tool call is pending — `404` where a name or a
route does not exist, `403` for a route whose audience excludes the caller's address, and
`500` where the operation failed. A refusal carries `success: false` and the reason in
`error`.

`403` is the only audience refusal, and it says one thing: the route was answered by
reaching this machine and this caller did not.

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
the page and the rest of the API, but not the list. A page that reached one over
the network gets `403`, keeps its own name from `/api/session`, and says in the header why it
has nothing to offer.

A sibling's address is that port on the host the page already reached, because the registry's
`bind` is where a listener sits and not a destination: `0.0.0.0` is a legal bind and never a
legal address to dial, and a server behind a proxy would name one this page cannot reach.

The page is one server per tab by nature — one socket, one transcript — but naming them is
not: the header shows the workspace this page is talking to, and lists the others when there
are any. Choosing one moves the page to it. Switching is deliberately light: the socket is
closed and reopened against the chosen server, the transcript is read again
from it, and the server that was left keeps running the turn it was given. That is why a turn
in progress holds the switcher: nothing here moves a running reply between servers, and
pretending otherwise would be the one case where a reader loses work.
