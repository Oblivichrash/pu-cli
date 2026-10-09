# Serving more than one session

Status: plan for work not yet started. The server drives exactly one session today;
this is what it would take to drive several, one per workspace directory, without
them interfering with each other.

## What a session is

A directory is a session. `<workspace>/.pu/agents.json` and
`<workspace>/.pu/session.json` already make a directory the unit of both
configuration and stored conversation, and `pu serve` is started inside one. Two
pages looking at the same directory are looking at the same conversation; two
directories are two conversations, and neither should be able to disturb the other.

## What stands in the way

`Runtime` holds one session and everything built around it:

| Held by `Runtime` | Why it is per session |
| --- | --- |
| `current_session_`, a `Session` of `Workspace` + `RuntimeSpec` | the conversation, the store file, and the agent and thinking level a restart resumes with |
| `AgentManager`, `Toolbox`, and their MCP child processes | `agents.json` is per directory, and its MCP servers are processes |
| `Executor` | holds the toolbox and the security policy it was built for |
| `CommandRouter` | takes the session it acts on as an argument, so one instance would do |

The serve layer adds one more: `io_mutex`, taken for the whole of every turn and by
every REST call. A long turn in one directory therefore blocks another directory's
turn and its status requests, which is most of what the change is for.

The REST surfaces are global too. `/api/session`, `/api/history`, `/api/clear`,
`/api/rewind`, `/api/agents`, `/api/agent/switch`, `/api/thinking` and
`/api/workspace/switch` all act on whichever session happens to be current, so a
workspace switch in one page moves the session out from under another. The WebSocket
has no notion of which session it is talking to at all: it talks to the only one.

## Shape

1. **A host per session.** Extract the per-session parts of `Runtime` into a
   `SessionHost`: the session, its agent manager, its toolbox and MCP processes, its
   executor, and its own mutex. `Runtime` keeps what is global — initialisation,
   shutdown, the store directory rules — and a map from canonical workspace path to
   host. The CLI uses a single host, as it does now.
2. **Every request names its session.** The page's identity is the directory: the UI
   opens `/?w=<path>`, the WebSocket connects to `/ws?w=<path>`, and the REST routes
   take the same parameter. `/api/workspaces` already lists the directories that can
   be opened.
3. **One mutex per host**, so a turn in one directory no longer blocks another's
   requests or its turn.
4. **Costs and limits.** Each host starts its own MCP servers, so sessions are not
   free: start lazily on first use, evict a host after it has been idle (the store is
   written after every turn and on shutdown, so evicting loses nothing), cap how many
   may be live at once, and list them through `/api/sessions`.
5. **The UI already has the switcher.** Switching workspace becomes navigation to
   another session rather than a global mutation, so opening a second directory in a
   second tab leaves the first one running.

## What a turn is, still

A turn belongs to the client watching it: within a session one client at a time is
written to, and a takeover or a disconnection ends the turn that was running. That is
what keeps a chat from being driven by nobody, and it applies per session rather than
per server.

## Alternatives

Running one `pu serve` per directory needs no code at all: the process boundary
already gives full isolation, MCP processes included. What it does not give is one
port and one page listing the directories, and it costs a process and a port per
workspace. If the need is "two directories at once, today", that is the cheapest
thing that works.

## Open questions

- Does the UI switch directories by navigating — a page load, with the directory in
  the URL so a session can be linked to — or in place?
- How many hosts may be live at once, and after how long idle may one be evicted?
- Two pages on the same directory: still one client at a time, with the second taking
  over and ending the first's turn, or is the second connection refused?
