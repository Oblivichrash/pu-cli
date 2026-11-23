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
| `web/state.js` | The `PU` object, the page's own identity and the list of siblings, the writers for both (`setSelf`, `setOthers`, `siblingOrigin`), the connection dot's state |
| `web/api.js` | The page's one way out: `fetchJson`/`postJson` over `PU.self.origin`, and `ApiError`, which keeps the status a refusal arrived with. A caller that cannot tell `403` from a dropped connection cannot say which happened |
| `web/block-thinking.js`, `web/block-tool.js`, `web/block-subtask.js` | The block custom elements and their own drawing |
| `web/blocks.js` | The block kinds, and which kind a tool call is drawn as |
| `web/block-node.js` | Picking the element a block type becomes, and the folding a block shares |
| `web/message.js` | The frame around a message: role label, container, the press a reader makes on one — a user turn goes back into the composer to be edited, an assistant turn goes to the clipboard — and the guards that keep that press from firing on a link, a tool card, or a passage the reader has selected. A press that acted silently would look like nothing happened, so a copy with no text to copy says so in a system message |
| `web/transcript.js` | The assistant message in flight, block patching, the chunk handlers |
| `web/approval.js` | The card parked above the composer while a tool call waits for an answer |
| `web/panel.js` | What the page reads rather than chooses: the backend label, the host field (drawn from the server's answer, and left alone while it has focus so a typing reader is not overwritten), the tool list, and the usage reading — the count and its hairline in the status bar, with this turn's figures and the conversation's total in that item's tooltip |
| `web/servers.js` | The header's server strip: the workspace this page is attached to, the other servers over `/api/servers`, and the switch to one of them. The name comes from `/api/session`, never from the list, so a refused list leaves the name intact and the caret explains why it cannot be used |
| `web/notice.js` | The strip above the composer |
| `web/history.js` | Folding stored messages back into the bubbles a reader saw |
| `web/ws.js` | The socket, built from `PU.self.origin`, its reconnect policy, the frame dispatch table |
| `web/session.js` | The REST calls: session, history, thinking, agents, backend choice, rewind |
| `web/config.js` | The configuration dialog: everything the reader writes rather than reads, opened from the gear in the header. It is a `<dialog>`, so the focus trap and Escape come from the browser; the one thing it adds is that opening it asks the gateway for its model list, since the menu is drawn from whatever that gateway serves |
| `web/composer.js` | The composer and everything it starts: the press's mark (an arrow while a turn is the thing the press would start, a square while one is running and the press would end it), the field growing to its own text, the one place a turn is sent from, and the edit flow — `beginEdit`, `cancelEdit`, `setComposerMode`, `sendEdit` — since an edit is a send whose text is a step back |
| `web/app.js` | The element lookups and the start-up order, and nothing else |

A transcript block and its element are separate things: the block is plain data (`type`,
`content`, `collapsed`, …) and the element is a custom element that draws itself from it
on connect. `sync(block)` is the only way in, so a patch and a first paint take the same
path, and the coalesced re-render in `transcript.js` only has to decide *which* blocks
bear re-reading rather than how to draw them.

**One button, one mark.** The send control is an SVG mark inside a filled accent circle in the
field's lower right corner, not a word beside it: a block of text was the loudest thing on a page
whose messages are paper and hairline, and it took a width the field could have used. The circle is
the one filled shape on the page, which is what makes a glyph in a corner read as something to
press rather than something to look at — an arrow is a direction, not a block of text, and a ring
around it is the form every send control takes. The mark itself is Lucide's `arrow-up` on the
library's own 24-unit grid, the same grid the gear in the header is drawn on, so the two controls
read as one hand. The grid and the hand are shared but not the scale: the gear is set to the 14-15px
of the text beside it, while this mark is cut at 18px — smaller than the 24px it was when it stood
alone, because inside a 32px ring a 24px arrow pressed against the edge and made the circle look
like a sticker; at 18px it keeps a ring's worth of light around it. The gear, by the same reading,
is not boxed — nothing else in the header is, and a frame around the one control drew a border
round a single word. While a turn is in flight the arrow gives way to a solid square — the
press that ends a turn is the same press, so the mark says what the *next* press does rather than
adding a second control. The square is filled and not
outlined for the same reason it is not a circle: at this size an outline a stroke wide reads as an
empty checkbox, and `circle-stop`'s inner square collapses into a blob. The words are not gone, only
moved: the button keeps its `aria-label` (`Send`, `Stop`, `Stopping`) and its `title`, so a screen
reader and a hovering pointer still hear which press this is. From the press until the server's
`done` the square dims and the button is `disabled` — a press still on its way is refused rather
than repeated. The frame it sends is `cancel`, which ends the turn and whatever command it is
running; a reader who presses it means both, so splitting the mark would offer a choice the
protocol does not make. `renderSendButton` is the one place those states are written, so a path
that ends a turn cannot leave the mark saying something the page is not doing. It toggles the
`hidden` *attribute* rather than the property, because on an SVG element `hidden` is only an
attribute the UA sheet happens to style and assigning `false` to it leaves the mark on screen.

That re-render is deferred to the next animation frame, so a fast stream costs one paint
rather than one per chunk — which makes the frame a thing that can be *owed*. Two rules
follow from it, and both are load-bearing:

- **Anything that stops the bubble being the live one draws first.** `done` arrives in the
  same tick as the last chunk, so a turn ended without a flush leaves its final text in the
  block list and nothing on the page: what a reader sees is a prefix of the answer the store
  holds, which is why a reload shows the rest and nothing looks wrong from the server's side.
  `finishAssistantMessage()` therefore flushes, and the paths that discard the bubble
  (`removeCurrentAssistantMessage`, an error frame, a server switch) cancel it instead.
- **A hidden tab is not painted**, so a redraw it is holding is delivered only when the reader
  comes back, and the reply stops at the point they left it until then. The page flushes on
  `visibilitychange` as it goes out of sight, so the DOM is current at the moment painting stops
  rather than at the moment it resumes.

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
