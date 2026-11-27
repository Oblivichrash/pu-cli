# Serve transport

Why `pu serve` is shaped the way it is. [web-api.md](../web-api.md) states the wire;
this states the reasoning behind it, which is longer than a reference section should
carry and is read once rather than looked up.

## A port is where a server is, not which one it is

`--bind` names the interface the listener sits on, not an address to open. `0.0.0.0` is a
legal bind and never a destination a browser can dial, so the start-up banner says which
port it took and leaves the address to the reader: the page names this server by the
address it was reached on.

A server serves the directory it was started in and stays there; nothing moves it to
another one. Each directory is a session with its own configuration, conversation and
tools, so serving several of them at once means one server per directory — which is also
what keeps them from sharing a conversation or a turn.

A port is what a reader has to remember to tell two servers apart, and the page is what
has to do it instead. That is the whole reason a registry exists: it is not a
configuration file and it is not state the rest of the program reads.

## The serve registry

Each server writes itself into `$HOME/.pu/serve.json` while it runs — directory, bind,
port, pid and the process's start time — and deletes the line on the way out. The page
lists the machine's servers and names each one by its workspace.

Four details, each of which is a bug that was in the design rather than in the code:

- **A record whose process has stopped is dropped on the next read.** That is what makes
  a crash cost a stale line rather than a ghost entry in a menu.
- **The start time is recorded because a pid outlives the program that held it.** Once
  its owner exits the number is handed to the next program, so a reader that asked only
  whether the pid was alive would get a true answer about something else and list a
  server nobody can reach.
- **Read and write happen under `serve.json.lock`,** held for the few milliseconds the
  update takes. Two servers starting together would otherwise each read the list before
  the other had written it, and the second write would drop the first.
- **The temp file each writer renames is named after its own process,** so a writer that
  gave up waiting on the lock cannot publish over one that finished.

The file is a record of what is running, not a setting: `serve` in `agents.json` is still
the port a workspace asks for, and the registry is what actually answers.

A machine with no `HOME` gets no registry, and therefore no list — the servers still run.

## One WebSocket, not REST polling

Chat is a WebSocket at `/ws`; everything else is REST, for control and status. The page
names itself on the upgrade (`/ws?client=<tab-id>`), so a reload and a reconnect are
recognisably the same page.

The single-socket rule is what makes cancel a fact rather than a race. A turn belongs to
the client watching it: closing the page or losing the socket ends it, and nothing of it
is kept — what it had written is half an answer, and storing that would make the next
request read it as the model's finished reply. A page that reloads mid-answer comes back
to the conversation as the store has it: the question, and no reply to it.

Two exceptions, both of which are a socket going away without the *page* going away: a
`cancel` the page asked for stops the turn but keeps what already arrived and ends with
`done`; and a socket the same page replaces — a reload, or the watchdog giving up on one
that went silent — is not the page leaving, so the turn runs on and the frames produced
during the swap are held for the socket that attaches next.

The read loop is what an asynchronous stream buys: a connection parked in a synchronous
read cannot be woken by closing its socket, so shutdown would wait on a client that will
never speak again. Here the run happens on the runner's own thread and the stream belongs
to the strand, so shutdown stops the `io_context` and every read returns at once.

## The single-page rule

The front-end is plain scripts with no build step, so the load order in `web/index.html`
*is* the dependency order and each file has one job. There is no bundler, no module
resolution step and no transpile: what the browser fetches is what the repository holds,
which means a stack trace in a page names a file a reader can open and the diff of a
change is exactly the change.

The cost is that a module cannot import another and must rely on load order, so the
ownership boundaries have to be written down rather than derived. [web-ui.md](web-ui.md)
is where they are.
