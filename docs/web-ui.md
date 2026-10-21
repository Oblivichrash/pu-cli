# The Web UI

The page `pu serve` hands out: the modules it is built from, and how the files reach a
browser. The socket protocol and the REST routes are in [web-api.md](web-api.md); this
document owns what the page itself is.

## Browser modules

The page is plain scripts with no build step, so the load order in `web/index.html` *is*
the dependency order and every module's entry points are globals. `web/state.js` runs
first and owns the one mutable object, `PU`: the socket, the origin it talks to, the
streaming flags, the assistant message being written, the chain length, and the composer's
elements. Every other module reads and writes `PU` rather than keeping a copy, so a fact
has exactly one home and no module has to be told about another's.

`PU.self` is the one address every request and the socket are built from, and it is never
unknown: a page was served by a server, so `origin` starts as the address that served this
document. It also carries the `name` and `port` the server reports for itself, which is what
the header draws. Switching servers writes all three together through `setSelf`, so the page
cannot label one server's transcript with another's name.

`PU.others` is the separate fact the switcher needs: the other servers on this machine, and
`PU.othersRestricted` says why it is empty when it is. Keeping the two apart is what stops a
page whose server list is unavailable — every page reached over the network — from losing its
own name.

| File | Owns |
| :--- | :--- |
| `web/state.js` | The `PU` object, the page's own identity and the list of siblings, the writers for both (`setSelf`, `setOthers`, `siblingOrigin`), the frame-coalesced render queue, the connection dot's state |
| `web/api.js` | The page's one way out: `fetchJson`/`postJson` over `PU.self.origin`, and `ApiError`, which keeps the status a refusal arrived with. A caller that cannot tell `403` from a dropped connection cannot say which happened |
| `web/block-thinking.js`, `web/block-tool.js` | The three block custom elements and their own drawing |
| `web/block-node.js` | Picking the element a block type becomes, and the folding a block shares |
| `web/message.js` | The frame around a message: role label, container, edit affordance |
| `web/transcript.js` | The assistant message in flight, block patching, the chunk handlers |
| `web/layout.js` | Which way the panel opens, and remembering that choice across the column and drawer layouts |
| `web/approval.js` | The card parked above the composer while a tool call waits for an answer |
| `web/panel.js` | What the sidebar reads rather than chooses: backend label, context bar, tool list, and the spend total a finished turn brings with it |
| `web/board.js` | The peer board's read-out over `/api/board`, and the three verbs the page writes: answering a blocked job, asking what is happening, and taking one back |
| `web/servers.js` | The header's server strip: the workspace this page is attached to, the other servers over `/api/servers`, and the switch to one of them. The name comes from `/api/session`, never from the list, so a refused list leaves the name intact and the caret explains why it cannot be used |
| `web/notice.js` | The strip above the composer |
| `web/history.js` | Folding stored messages back into the bubbles a reader saw |
| `web/ws.js` | The socket, built from `PU.self.origin`, its reconnect policy, the frame dispatch table |
| `web/session.js` | The REST calls: session, history, thinking, agents, rewind |
| `web/composer.js` | Turning a press into a run, and a second press into a cancel |
| `web/app.js` | The element lookups and the start-up order, and nothing else |

A transcript block and its element are separate things: the block is plain data (`type`,
`content`, `collapsed`, …) and the element is a custom element that draws itself from it
on connect. `sync(block)` is the only way in, so a patch and a first paint take the same
path, and the coalesced re-render in `transcript.js` only has to decide *which* blocks
bear re-reading rather than how to draw them.

The block list is a timeline, in the order the turn happened: a reasoning chunk, a tool
call and a further reasoning chunk are three blocks, not one merged by kind. A new chunk
joins the block it follows only when that block is the same kind, which is the rule both
`transcript.js` and `history.js` apply, so a replayed history and a live stream are the
same shape.

A block that folds says so by setting `collapsible`, and `block-node.js` builds and
re-reads the header, the body and the arrow for it; an element that folds is not named
anywhere else.

Whether the view follows the reply is a question asked of the transcript element at the
moment of use, never a stored answer. Content grows under the reader without any scroll
event, so a flag set only by scrolling goes stale the instant a bubble is appended: it
then reads "the reader has scrolled away" while the reader is still at the bottom, and
the reply stops following for the rest of the turn. `isAtBottom()` therefore measures the
element each time it is asked, and a writer reads it *before* it appends, because the
append is the very thing that moves the content being measured. A reader who scrolls up
is left where they are; returning to the bottom resumes following with no other gesture.

## How the page is served

The UI travels with the binary, not with the workspace the session is pointed at, so
`GetWebDir` resolves the page directory in this order and takes the first that exists:

| Order | Candidate | Why |
| :---- | :-------- | :-- |
| 1 | `$PU_WEB_DIR` | The one explicit override, for a checkout whose layout we cannot guess |
| 2 | `<executable>/web`, `<executable>/../web`, `<executable>/../share/pu/web` | A build tree or an install keeps the page under its own prefix, and a workspace somewhere else must not hide it |
| 3 | `./web`, `../share/pu/web`, `/usr/share/pu/web`, `/usr/local/share/pu/web` | Release and system installs, where the page sits beside the working directory |

Only the files in the page directory are served. A target that names a directory, or one that
is not there, is answered with the `404` that ends `DispatchHttpRequest`, so the route cannot be
walked to read anything outside the page directory — and a new `web/*.js` needs no change on
the server side, since the directory is the list.

The target is joined to the page directory with `operator/`, after the leading slash is
dropped and the bare `/` is mapped to `index.html`. Concatenating the strings instead
would make the result depend on whether the configured directory happens to end in a
separator, and would misreport a correctly configured but separator-less directory as a
missing page.

On macOS the executable's own path is reported by `_NSGetExecutablePath`, which fills a
buffer whose size it also reports and may leave `..` in the result, so it is resolved
before its directory is taken.
