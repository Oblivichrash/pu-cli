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
| `web/panel.js` | What the page draws from the server's answer: the gateway and the model as two separate readings in the status bar (two, because they are two choices side by side, and a press on one must not be read as a press on the other), the tool list, and the usage reading — the count and its hairline, with this turn's figures and the conversation's total in that item's tooltip |
| `web/servers.js` | The header's server strip: the workspace this page is attached to, the other servers over `/api/servers`, and the switch to one of them. The name comes from `/api/session`, never from the list, so a refused list leaves the name intact and the caret explains why it cannot be used |
| `web/notice.js` | The strip above the composer |
| `web/history.js` | Folding stored messages back into the bubbles a reader saw |
| `web/ws.js` | The socket, built from `PU.self.origin`, its reconnect policy, the frame dispatch table |
| `web/session.js` | The REST calls: session, history, thinking, agents, the gateway and model writes (`saveGateway`, `saveModel`), rewind |
| `web/gateway.js` | The two forms the status bar opens: the **gateway form** at `#gateway-dialog` (which gateway, and the address it is reached at) and the **model form** at `#model-dialog` (which model that gateway answers with, and how — temperature, thinking, max tokens, top P, seed, context window). Both are `<dialog>`s, so the focus trap and Escape come from the browser, and both close only when the server took the answers: a refusal is the form's to show, and a form that closed on one would leave the reader believing a change that was never made |
| `web/config.js` | The configuration dialog: everything the reader writes rather than reads, opened from the gear in the header. It holds the agent's name, the session settings that are not a model's (the tool budget), and the tool list — the gateway, the address, the model and everything a model answers with belong to `gateway.js`'s two forms. It is a `<dialog>`, so the focus trap and Escape come from the browser |
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
of the text beside it, while this mark is cut at 16px — smaller than the 24px it was when it stood
alone, because inside a 28px ring a 24px arrow pressed against the edge and made the circle look
like a sticker; at 16px it keeps a ring's worth of light around it. The ratio of mark to ring is
what is held constant, not either figure alone: a filled disc reads heavier than an open glyph, so
the ring came down with the arrow rather than staying at the size a glyph would have needed. The gear, by the same reading,
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

**A model is a form, not a menu.** The gateway answers the question *where the turn is sent*; the
model answers *how it is answered*, and the second is more than a name — the same gateway will
answer at any temperature, at any reasoning level, under any cap, against any window. The status
bar therefore opens two dialogs: the gateway form holds the gateway and its address, the model form
holds the name and everything the model answers with — the sampling settings and the thinking
level. Both names are menus, and for the same reason. Which gateways exist is the build's fact,
and which models a gateway serves is the catalogue's: in each case the rows are the answer, and a
name outside them is a typo the field would have to be corrected out of. A `<datalist>` was tried
for both and is not the same control — Chromium stops offering a list once the box holds a value
the list already matches, which is exactly the state a form opens in, so the reader meets a field
that looks closed. The model menu is built from the account's whole catalogue, with any name an
agent lists but the catalogue omits appended: those are two different questions — what the account
is offered, and what one agent may run — and the menu answers the reader's, which is the first.
The model menu always lists the name in force, drawn first when the catalogue does not carry it: a
gateway answers names it does not advertise, and a catalogue that could not be read must leave the
field holding what runs rather than empty. Two controls for one answer would be the fault; one
control with the right shape is not.

A catalogue can also be empty, and that is a state the gateway states rather than a failure: one
account answers `"agents":null,"models":null`, meaning it has no models configured. A menu of
nothing is a dead end — nothing to pick and no way to type out of it — so in that case, and only
that case, the menu gives way to a box the reader names a model in, prefilled with the name in
force. `#md-error` says which of the two emptinesses it is: a catalogue that could not be read
names the failure and invites a name; a catalogue that answered and listed nothing says the
account is empty. The two controls are never both on screen, and which one holds the answer is
decided once, when the fill happens. What *is* free here is the address:
the menu's answer only fills it with that gateway's default, and the reader may then edit it — a
proxy, a local mirror, a gateway on another port. A setting left empty means the gateway's own
default and is written as `null`; clearing `temperature` restores the default rather than removing
the key, because the backend always answers at some temperature. The level is a menu rather than a
box and has no empty state: `auto` is the reader's word for "no level of our own" and travels as
itself, and the file spells it `default`. A refusal — `'top_p' must be above 0 and at most 1` —
keeps the form open with the reason in `#md-error`.

The sampling settings and the level are also session overrides, and an override outranks the agent
file. A save therefore writes the file *and* clears the `temperature`, `max_tokens` and `thinking`
overrides, so the value just saved is the value in force; without that second step the form would
report success while the reader went on seeing the override's value.

**The address follows the gateway.** An address is a fact about the gateway it was typed for, not
about the form, so the box is rewritten the moment the gateway menu changes: `/api/gateways` carries
each gateway's default address, and the new one replaces whatever the box held. Filling it once, from
the gateway in force, was wrong in exactly the case the form exists for — switching away from that
gateway — and it was wrong quietly. The box went on showing `http://localhost:11434` while the menu
said `deepseek`, and the save posted that address to a vendor that does not answer there: the model
was fine, the route was not, and nothing on screen said so. The box is still the reader's, so a value
they typed is theirs to keep — it is replaced only by the gateway change itself.

**A save says it is working.** Choosing a backend asks the new gateway what it serves before it
posts, and a gateway that cannot be reached takes the whole connect timeout to say so — ten seconds
during which a button that looked untouched would be read as one that does not work. Both save
buttons therefore take `Saving…`, `disabled` and `aria-busy="true"` for the duration and give the
label back afterwards, whether the save succeeded, was refused, or timed out. The button is also
guarded on `aria-busy`, so a second press while the first is still in flight does nothing.

**The three sheets are one shape.** A `<dialog>` arrives from the browser with a 3px solid black
border, a square white box and 16px of its own padding. That is close to what this page wants and
not the same thing: black is not ink here and white is not paper, so a sheet drawn that way reads
as a different kind of object from every other surface on the page. The configuration sheet was
written out by hand and the two backend forms were not, so they inherited the browser's box and sat
beside it in a visibly different style. The shape is therefore written once, on `dialog` by element
rather than by id, so a fourth sheet added later cannot miss it the way the second and third did.

**A backend form does not name its agent.** Both forms briefly ended with a line reading
`Agent default`, on the reasoning that each changes one agent's backend and the reader might not
know which. In practice the line could only ever say one thing: a served process has exactly one
session and therefore one agent, so the name is a constant on a page whose subject is that agent,
and a constant is not information. It is gone. The configuration sheet still states the name,
because that sheet is about the agent itself.

**A form has one way out.** The gateway and model sheets end on `Cancel` and `Save`, and the `✕`
that used to sit in their corner is gone. On a configuration sheet with a scroll box and no buttons
that corner is the only way out and it stays. On these two the corner and the foot were both on
screen at once, doing the same thing, so the reader was left working out what separated them —
and a control whose meaning has to be worked out is a control that should not be there. `Cancel`
is the one visible exit; Escape and a backdrop click remain, and neither is drawn as a button.

Two consequences worth knowing. The heads of the two backend forms are now a title on its own row,
so the title style moved from `#config-title` to a `.config-title` class on all three — the same
lesson as the `dialog` rule above, in a smaller place: an id selector written for one sheet does
not reach the others. And an Escape pressed while a `<select>` has focus is still the browser's,
closing the open menu rather than the sheet, so the first Escape after opening a form may only
shut a dropdown; that is deliberate and was true before this change too.

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
