# Serving more than one session

Status: **not the route this branch takes.** A session is a directory, and several
directories are served side by side as several `pu serve` processes, one per port: the
process boundary already gives the isolation this would have had to build, down to the
toolbox, the MCP child processes and the store. What that costs is a process and a port
per workspace and no single page listing them, which the workspace's own `serve` block
makes workable — a directory can say which port it answers on. What follows is kept
because the shape, and the reason it was set aside, are worth having on record.

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
| `current_session_`, a `Session` of a `Conversation` + its `SessionSpec` | the conversation, the store file, and the agent and thinking level a restart resumes with |
| `AgentManager`, `Toolbox`, and their MCP child processes | `agents.json` is per directory, and its MCP servers are processes |
| `Executor` | holds the toolbox and the security policy it was built for |
| `CommandRouter` | takes the session it acts on as an argument, so one instance would do |

The serve layer adds one more: `io_mutex`, taken for the whole of every turn and by
every REST call. A long turn in one directory therefore blocks another directory's
turn and its status requests, which is most of what the change is for.

The REST surfaces are global too. `/api/session`, `/api/history`, `/api/clear`,
`/api/rewind`, `/api/agents`, `/api/agent/switch` and `/api/thinking` all act on
whichever session happens to be current. The WebSocket has no notion of which session
it is talking to at all: it talks to the only one. (The one request that could move a
session, `/api/workspace/switch`, has since been removed — see the status above.)

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

## What was decided instead

- One process per directory, not several sessions per process.
- A session cannot be moved: `/api/workspace/switch` is gone, so a server serves the
  directory it was started in. `/api/workspaces` stays, as a list of what exists.
- One page per session: a second connection is answered with `busy` rather than taking
  over, because taking over would mean ending the reply the page already there is
  reading.
- No cap on live sessions and no eviction: a process per session already bounds what
  one server holds, and the operating system bounds the rest.

The process boundary gives full isolation for free — the session, the toolbox, the MCP
child processes and the store are all that process's own. What it does not give is one
port for everything and one page listing the directories, and it costs a process and a
port per workspace, which the workspace's `serve` block covers by letting a directory
name its own.
