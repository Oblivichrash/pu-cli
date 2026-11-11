# What is left

The work still open on this build, ordered, plus what has been decided against so it is not
re-proposed. Everything else this document once described has either been built — paging,
elision, a pager that outlives the toolbox — or turned out to describe a tool this build does
not have.

## 1. The three pools, and the one still unbounded

A transcript grows in three ways that need three answers:

| Pool | Grows with | Bounded today |
|---|---|---|
| prose — user text, `content`, `reasoning` | turns | no, but physically slow |
| observations — `ToolPayload::content` | bytes read | yes: page cap + elision |
| scaffolding — `tool_calls[].id/.name/.arguments` | number of calls | **no** |

Scaffolding is the pool nobody budgets for. Ids and names cost 50–80 tokens per call before
the arguments start, and an argument body is never elided: twenty `write_file` calls of 200
lines each are 4000 lines re-sent every turn, forever, on the input side of a call whose
result was elided on the output side.

Four tiers, applied per node, monotone — a decision may only move down:

| Tier | What survives | Enters when |
|---|---|---|
| T0 live | the whole text | newest run for its source, or a page read deliberately after |
| T1 receipt | verdict, `page`, `buffer`, `extent`, `superseded_by` | a newer run of the same source was read |
| T2 tombstone | `success`, `blocked`, `error`, `refusal`, `stored`, `buffer.{id,source}`, `elided` — no cursor | it is a superseded receipt and more than the newest six receipts are |
| T3 gone | nothing | it is one of everything but the newest ten tool-call pairs |

All three exist. T2 is `paging::TombstonedReceipt`, spent by `TombstoneOldReceipts` in
`src/session/request.cpp`; T3 is `RetireOldToolCalls` in the same file, and it runs last because
it is the only tier that removes anything and every tier above it is expressed in terms of a
call that is still there. T2 is worth its ~15 tokens because what stops a model repeating a
failed approach is knowing it tried: **compress along the body axis, never along the
argv/verdict axis.** T3 goes in whole pairs, oldest first, because a `tool` message whose
`tool_call_id` names no call is invalid on every provider.

T3 keeps ten pairs, not six, and the gap is load-bearing. T2 fires on a superseded receipt that
is older than the newest six, so a T3 that also kept six would delete the receipts T2 was about
to shrink and leave T2 with nothing to do: the two tiers would silently collapse into one. Four
of headroom leaves two superseded receipts past the cursor floor in the common case, which is
what keeps both tiers observable at once.

The entry condition for each tier is **a count, not a size**. An earlier draft drove T2 from a
byte budget (`256 KiB` of accumulated tool output). That was wrong twice over: a byte count
cannot be turned into tokens without the model's tokenizer, so the threshold was a number
nobody could check against the window it was meant to protect; and because the newest six
results were held regardless, it was not a hard cap either — setting it to 4 KiB would have
silently failed to take effect. Supersession is a fact about the conversation rather than a
measurement of it, and "the newest six superseded receipts keep their cursor" is a rule that
can be stated, tested and read back. Six is not a tuned number; it is the length of a
plan-and-verify cycle, and the cost of being wrong is one round of rereading.

The table first said `command` and `cwd` survive T2, and both are wrong as written: a paged
result is a `paging::Envelope`, and that envelope holds `page`, `buffer` and `extent` and
nothing else — no verdict, no command, no working directory. The command a model repeats is
in the assistant's `tool_calls` one message above, which T2 never touches. So a tombstone
keeps a `verdict` or a top-level `command` when the receipt happens to carry one (a raw
`execute_bash` result does, a page does not) and otherwise keeps what the envelope has. This
is the argv axis honoured by *not compressing what is there*, rather than by inventing a field
to preserve.

## 2. The queue

1. ~~**A per-turn budget in the projection** (`request_bytes`, then `elided: "budget"`).~~ Done,
   then undone deliberately. The projection is the right place for this work — it is pure and
   rewind-safe — and it is where the tiers now live. But the *budget* framing was removed: what
   drives a receipt down a tier is supersession, not a byte count. See §1 for why a byte
   threshold was the wrong measure, and note that the tool-call budget in the executor is a
   different mechanism with a different subject: it bounds how many calls one turn may make,
   not how much history is re-sent. Nothing was removed from the history to achieve this — a
   receipt keeps its place and loses only the cursor.
2. ~~**`find` over a retained buffer.**~~ Done. `execute_bash` takes `query`, which searches
   the stored run without executing anything and reports each hit with its own line number so
   it can be followed up with `at`. It is a substring search rather than the predicate form
   sketched here, because a regex dialect inside a tool that already has a shell is a second
   language with its own bugs; a model that wants a pattern can run `grep`. The count-plus-range
   answer also landed: a search reports `found`/`shown`/`truncated` rather than pretending to be
   a page, because a noisy match set is not a window onto anything.
3. **`page` reaches the transcript a person reads.** `on_end` drops the envelope, so a page of
   a 3500-line log looks exactly like a whole answer. The `stored` counts are what say which
   it is.
4. ~~**T3: retire the oldest tool-call pairs.**~~ Done. `RetireOldToolCalls` removes whole
   pairs, oldest first, keeping the newest ten. The bound is ten rather than six so that T2
   still has receipts to tombstone; see the note under the tier table. `stored` arguments are
   still re-sent, which is item 5.
5. **Large `arguments` stored by reference** into the pager, for the same reason results are.

Ordering is not a preference: §2.5 is where the remaining growth actually is, so it comes
before the items that make reading history cheaper. Both are measured by `usage.prompt_tokens`
per turn and `tool_call_count` per turn.

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

