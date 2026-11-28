# What is left

The work still open on this build, ordered, plus what has been decided against so it is not
re-proposed. Everything else this document once described has either been built — paging,
elision, a pager that outlives the toolbox — or turned out to describe a tool this build does
not have.

## 0. Five CLI defects, still open

Found by driving four agents from a shell at 0.5.1 and re-checked against 0.7.3 by grepping
the source rather than the binary, so these are claims about this tree and not about a
release note. Each names the observable, because each is otherwise easy to read as intended
behaviour.

- **`pu ask` appends to the workspace conversation.** Three consecutive runs in a fresh
  directory leave 4, 6, 8 nodes. There is no stateless mode: `ask` is "continue this
  workspace's conversation with one more turn". For a scripted probe that is both a
  correctness problem (the model sees history it did not expect) and a growth problem.
  `--fresh` — an empty conversation, nothing saved — is the fix, and it is also right for
  `probe`, which captures a raw stream and has no business editing a conversation.
- **Read-only commands touch the workspace.** `pu models` in an empty directory leaves
  `.pu/logs/pu.log` and `.pu/session.json`, and rewrites the session even when the bytes are
  identical. `Runtime::Shutdown()` calls `SaveCurrentSession()` with no condition. Sharing
  this fix with the one above: who may write the session.
- **A prompt starting with `/` cannot be sent.** `ProcessInput` treats any leading `/` as a
  command, and `ask` has no escape — not even `--`. Asking about a path is an ordinary thing
  to do. In `chat`, where commands exist, a leading `/` should stay a command; in `ask` it
  should be a prompt.
- **No stdin prompt.** `echo hi | pu ask` is an error and `pu ask -` sends the literal `-`,
  so `pu ask "$(cat task.md)"` is the only route and it breaks on argv length and on quotes.
  A non-tty stdin should be the prompt.
- **`--agent` is persistent when it reads as per-run.** `--agent` is documented as "agent to
  use" and behaves as "agent to use from now on": `pu models --agent X` silently re-points
  what the next bare `pu ask` runs on, and a *failed* run writes it too. `ask` no longer
  writes a session, so `models` is where it still stands. This is the one item that is a
  breaking change to fix, which is why it is last: the split is `SetRunAgent` (this process)
  against `SetWorkspaceAgent` (what `/backend` and chat do), and the session should outrank
  the file's agent name once it is a deliberate choice rather than a side effect.

The first two are one question and should go together. The original evidence — commands,
outputs, and the paths that have moved since — is in
[cli-agent-workflow.md](cli-agent-workflow.md).

## 1. What a request carries

A transcript grows in two ways, and the projection answers both:

| Pool | Grows with | Bounded today |
|---|---|---|
| prose — user text, `content`, `reasoning` | turns | no, and deliberately |
| observations — `ToolPayload::content` | bytes read | yes: page cap + supersession |

Two tiers, applied per node, monotone — a decision may only move down:

| Tier | What survives | Enters when |
|---|---|---|
| live | the whole text | newest run for its source, or a page read deliberately after |
| receipt | verdict, `page`, `buffer`, `extent`, `superseded_by` | a newer run of the same source was read |
| tombstone | `success`, `blocked`, `error`, `refusal`, `stored`, `buffer.{id,source}`, `elided` — no cursor | it is a superseded receipt and more than the newest six receipts are |

All three exist. The receipt is `paging::SupersededReceipt`; the tombstone is
`paging::TombstonedReceipt`, spent by `TombstoneOldReceipts` in `src/session/request.cpp`. The
tombstone is worth its ~15 tokens because what stops a model repeating a failed approach is
knowing it tried: **compress along the body axis, never along the argv/verdict axis.**

Neither tier removes a message, so the transcript that reaches the model is the transcript that
was stored — same order, same length. That is not an oversight. A turn that no longer fits is a
turn the caller should have delegated rather than a turn the projection should have cut: a
subagent answers the question in its own context and hands back the conclusion, which is what
keeps the bulk of the work out of this one. **No pool is bounded by deleting history.**

The entry condition is a count, not a size. A byte count cannot be turned into tokens without
the model's tokenizer, so any threshold expressed in bytes is a number nobody can check against
the window it is meant to protect. An earlier draft drove the tombstone from a byte budget
(`256 KiB` of accumulated tool output) and that was wrong twice over: the threshold could not be
checked against a window, and because the newest six results were held regardless it was not a
hard cap either. Supersession is a fact about the conversation rather than a measurement of it,
and "the newest six superseded receipts keep their cursor" is a rule that can be stated, tested
and read back. Six is not a tuned number; it is the length of a plan-and-verify cycle, and the
cost of being wrong is one round of rereading.

The table first said `command` and `cwd` survive the tombstone, and both are wrong as written:
a paged result is a `paging::Envelope`, and that envelope holds `page`, `buffer` and `extent` and
nothing else — no verdict, no command, no working directory. The command a model repeats is
in the assistant's `tool_calls` one message above, which the tombstone never touches. So a
tombstone keeps a `verdict` or a top-level `command` when the receipt happens to carry one (a raw
`execute_bash` result does, a page does not) and otherwise keeps what the envelope has. This
is the argv axis honoured by *not compressing what is there*, rather than by inventing a field
to preserve.

## 2. The queue

1. ~~**A per-turn budget in the projection** (`request_bytes`, then `elided: "budget"`).~~ Done,
   then undone deliberately. The projection is the right place for this work — it is pure and
   rewind-safe — and it is where the tiers now live. But the *budget* framing was removed: what
   drives a receipt down a tier is supersession, not a byte count. See §1 for why a byte
   threshold was the wrong measure. Nothing was removed from the history to achieve this — a
   receipt keeps its place and loses only the cursor.
2. ~~**`find` over a retained buffer.**~~ Done. `execute_bash` takes `query`, which searches
   the stored run without executing anything and reports each hit with its own line number so
   it can be followed up with `at`. It is a substring search rather than the predicate form
   sketched here, because a regex dialect inside a tool that already has a shell is a second
   language with its own bugs; a model that wants a pattern can run `grep`. The count-plus-range
   answer also landed: a search reports `found`/`shown`/`truncated` rather than pretending to be
   a page, because a noisy match set is not a window onto anything.
3. ~~**`page` reaches the transcript a person reads.**~~ Done. `on_end` drops the envelope, so
   a page of a 3500-line log looked exactly like a whole answer; `block-tool.js` now renders
   `lines A–B of N` and, when something was dropped, which end was kept, and `history.js`
   carries `page` through replay so a replayed turn draws like a live one. The JavaScript
   itself is unverified: this machine has no JS runtime, so the field names were traced from
   `paging.cpp` to `block-tool.js` by hand and the wire contract is covered by a serve-stack
   test, but nothing has parsed or executed the front end.

Both are measured by `usage.prompt_tokens` per turn and `tool_call_count` per turn.

## 3. Two rungs further out

**A process layer first.** `exec::Run` merges stderr into text and cannot carry a binary frame;
a child with two binary pipes and a killable lifetime is the shared prerequisite for both of
the following, so it is built once.

**Calling a C library.** In-process `dlsym` is the obvious implementation and the wrong default:
a segfault takes the session with it, a blocking call cannot be cancelled, and a bad pointer
from the model corrupts the harness's heap. So a worker process per library: `fork` per call by
default (copy-on-write, ~0.5 ms, crash isolation for free), a long-lived stateful worker only
for libraries that need handles. The surface is three tools — `lib.list` with a query,
`lib.describe` for one symbol on demand, `lib.call` taking a batch — because every tool schema
is sent on every turn, and a catalogue of 400 functions in the system prompt is the thing this
design exists to avoid. Bodies never travel through the model: a handle names the data, the
way a cursor names a page.

**A pty.** `forkpty` behind `tty: true` or `pu run -- <cmd>`, never the default for
`execute_bash`: a pty turns colour on, line buffering off, `isatty()` true and stdin readable,
so a program that waits for input now blocks. Set the size before the child draws, and strip
what the terminal adds (`\r` before `\n`, the echo of anything typed). On Windows, ConPTY is
the part with no prior art in this tree.

## 4. Reading a terminal: drafted, not scheduled

Three facts decide all of it, and they are not going to change:

1. the screen is not in the kernel — the grid and scrollback live in the emulator's memory;
2. a pty master has exactly one consumer, so duplicating the fd is a takeover, not an
   observation;
3. the screen is a pure function of the byte stream, so replaying it through our own parser is
   the correct way to see what a user sees.

The ranking that follows: a relay pty for the general case, shell hook and D-Bus identity
always on, an eBPF uprobe only when a live tab must be watched without a restart, and no
`ptrace`/`pidfd_getfd` reading of an emulator's internals at all. The first target is tmux,
because "page position" cannot come from Konsole and `capture-pane -e` gives the composed grid
with colour — a user scrolled up 200 lines is the strongest intent signal available. A field
with no reader is absent, not defaulted.

None of this is scheduled. What it would need: pty capture, a tmux provider, a shell hook, a
Konsole identity read. The protocol to resume from rather than restart is line-delimited JSON
on stdio, a closed field vocabulary, one snapshot per turn, merged by field.

## 5. Not to build

- **A templated `Viewport<T>`.** `Slice` returns bytes and the envelope is JSON; a template
  argument would leak into the wire format.
- **A tool per library, or schemas generated from headers.** The transport sends every schema
  every turn, and `RegisterTool` would mangle 400 names into `lib_foo_1`, `lib_foo_2`.
- **Image tiles.** Leave the unit in the enum for a uniform envelope; nothing consumes a crop.
- **Two message lanes.** The protocol rejects an orphan `tool` message, so the smallest legal
  removal unit is the pair, and the merge point would be this projection one indirection later.
- **Summarising per turn.** Nondeterministic, latency-doubling, cache-hostile, untestable. If a
  summariser is ever added: labelled, reversible through a cursor, computed once.
- **A token budget enforced by counting.** See §2.1: substitute, do not guess.
- **Scraping another process's pty, OCR of a screenshot, a global pointer→cell map, Konsole
  plugins, `TIOCSTI`.** Each buys back a computation worth doing properly, at a privacy or a
  version-lock cost.

