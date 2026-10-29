# What is left

Seven design notes used to sit beside this one. Their reasoning is in the commits that wrote
them; what remains here is the part that is still true about the code as it is, ordered, plus
the list of things already decided against. Anything not mentioned was either built — paging,
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
| T2 tombstone | `command`, `cwd`, `exit`, line count, buffer id — no cursor | the pool is over budget and the pair is older than the newest K |
| T3 gone | nothing | scaffolding is over budget |

T0 and T1 exist. T2 and T3 do not. T2 is worth its ~15 tokens because what stops a model
repeating a failed approach is knowing it tried: **compress along the body axis, never along
the argv/verdict axis.** T3 goes in whole pairs, oldest first, keeping the newest K (≈6 covers
a plan-and-verify cycle), because a `tool` message whose `tool_call_id` names no call is
invalid on every provider.

## 2. The queue

1. **A per-turn budget in the projection** (`request_bytes`, then `elided: "budget"`). The
   projection is the one place removal is provably safe, and it is already pure and
   rewind-safe. A running *token* counter does not work: it has to guess which message is
   disposable and breaks the call/result pairing.
2. **`find` over a retained buffer.** `grep` today is a shell command, so searching means
   *executing again*: it re-runs the build, its output can disagree with the page already in
   hand, and the retained page is not searched at all. The same cursor space should answer two
   access modes — `o17:400` (positional) and `o17 ? "error"` (predicate). Search results are
   rows, not lines, so a noisy one needs a count-plus-range answer ("3000 matches, here are
   20"), not a page.
3. **`page` reaches the transcript a person reads.** `on_end` drops the envelope, so a page of
   a 3500-line log looks exactly like a whole answer. The `extent` is what says which it is.
4. **Large `arguments` stored by reference** into the pager, for the same reason results are.

Ordering is not a preference: without elision, `find` results accumulate the way pages did.
Every step is measured by `usage.prompt_tokens` per turn and `tool_call_count` per turn.

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

None of this is scheduled. What it would need — pty capture, a tmux provider, a shell hook, a
Konsole identity read — is in §4 of the commits that wrote it, and the plugin protocol drafted
there (line-delimited JSON on stdio, closed field vocabulary, one snapshot per turn, merge by
field) is where to resume rather than restart.

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

## 6. Adjacent, worth a commit each

- `RiskLevel::kSafe`/`kNeutral` are computed and never read; `dangerous_patterns_` and
  `safe_commands_` are a policy with no entry in the documentation.
- `peers-and-board` is frozen, and [peers-and-board.md](../peers-and-board.md) says what that
  means: the board is drawn, but every verb behind it needs two servers and a `peers.json` to
  exercise, and there is no product direction to justify extending it.
