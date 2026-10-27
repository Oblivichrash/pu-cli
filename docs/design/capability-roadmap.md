# What to build next, given this codebase

`context-viewport.md` and `native-library-calls.md` describe where the capability
should end up. This document is the part that depends on the code as it is today:
which seams already exist, which three properties of the current design decide the
order of work, and what is not worth building at all.

**Conclusion up front:** the pager is bounded *per call*, and the conversation is
not bounded *per turn*. Fixing that is worth more than the abstraction, and it is
also the cheapest thing on the list — so it comes first, and the abstraction second.

---

## 1. Seams this code already gives you

Three things are already in the right shape and should be used rather than replaced.

| Seam | Where | Why it matters here |
|---|---|---|
| `BuildRequestPath(graph, inputs)` is a pure projection | `include/pu/session/request.hpp` | every turn's wire form passes through **one** function that never mutates the graph — the natural place to elide what the model no longer needs, and rewind-safe by construction |
| `ToolContext` carries injected capabilities | `include/pu/tools/tool.hpp` | `security`, `control`, `approval` are pointers to session-scoped state, so a capability costs a field rather than a new signature. The pager is the exception and took the other route: `Runtime` holds the `Pager` and hands it to the tool that reads it, in that tool's constructor |
| `paging` is an agent-level config block | `include/pu/config/agents.hpp`, `include/pu/paging/paging.hpp` | per-agent tuning exists and is parsed with an eye to bad values; the remaining knobs go beside it, not into a new file |

`ToolPayload.content` is an opaque string in the session (`include/pu/session/message.hpp`), so
a richer `window` object needs **no** session schema bump — the version guard in
`Runtime::LoadSessionFromFile` stays as it is.

## 2. Three properties that decide the order

### B1. The pager dies with the tool object, not with the session

`BashTool` owns `OutputPager pager_` (`include/pu/tools/bash.hpp`), and
`Runtime::RebuildToolbox()` calls `toolbox_.Clear()` before re-registering
(`runtime.cpp:334`). `RebuildToolbox` runs on `Initialize`, on `/agents`, and on
`/backend` — so **switching model or agent silently invalidates every cursor**, and
`SwitchBackend` is a routine action, not an edge case. The README promises a cursor
survives "until its buffer is dropped (the last 8, within 4 MiB)"; in fact it dies on
every backend switch.

It also means `read_file`-style tools cannot share a pager even if they wanted to:
the store is private state of one tool instance.

**Fix:** the store belongs to the session. Add `ViewportStore` to `Runtime` (or to
`Session`), pass it through `ToolContext` the way `security` is passed, and let a
tool borrow it. Eviction then has one owner and one policy instead of one per tool.

### B2. Every page is stored verbatim and re-sent forever

`RunToolLoop` appends each tool result as-is (`executor.cpp:362`), and
`BuildRequestPath` renders every node in `graph.Chain()` with no filtering. So a
paged exploration of a log is not bounded by the page size: 30 pages is 30 messages,
all of them in the next request, all of them in `session.json`.

This is the gap `bash-output-window.md` already names in §5 ("the conversation is
still unbounded"), but it is bigger than that section implies: the tool bounds
*one* result, and the model's natural remedy — page again — is exactly what grows
the transcript. `execute_bash` makes each mistake smaller; it does not make the
sequence finite.

**Fix: elide in the projection, not in the tool.** When a later tool result names the
same buffer, earlier results from that buffer can be replaced in the *request* with a
one-line receipt:

```json
{"success": true, "output": "", "error": "",
 "elided": {"source": "o7", "shown": [3505, 3520], "note": "page replaced by a later read"}}
```

Rules that keep it honest: only tool results, only the same `source`, only when the
later result is in the chain after it, always leave the last one intact, and never
touch the graph. The session keeps the full history for rewind and for a future
"show me what you actually read"; the model sees the last page plus a pointer.

This is a projection rule of about 30 lines and it is the single highest-value
change on this list. It also needs no consent from the model, which is what makes it
different from a guideline in the system prompt.

### B3. The tool loop caps at 20 iterations

`RunToolLoop` stops after `max_iterations = 20` (`executor.cpp:193`). A strategy of
"page until you find the error" spends that budget on I/O and then fails the turn
with "narrow the scope". `view=tail` was chosen because it needs one call; any
further paging is expensive by construction.

**Consequence:** `find` over a retained buffer is not a convenience, it is the
feature that makes the pager usable. One call that returns `[line, text]` pairs beats
N cursor calls and N stored messages, and it keeps B2's transcript flat.

## 3. The order that follows

| Phase | Change | Touches | Why here |
|---|---|---|---|
| **0** | The pager on `Runtime`, handed to the tool that reads it | `include/pu/runtime/runtime.hpp`, `src/runtime/runtime.cpp`, `include/pu/tools/bash.hpp` | B1: a cursor must survive a model switch before any of it is worth documenting |
| **1** | Projection elision of superseded pages | `session/request.hpp` (+ test) | B2: the only lever that bounds a *turn*, and it is pure |
| **2** | `find` on a buffer; `page` reaches the UI | `src/tools/bash.cpp`, `src/runtime/executor.cpp`, `web/app.js` | B3, plus: today `on_end` drops the envelope, so a person reading the transcript cannot tell a page from a whole answer |
| **3** | `core/process.hpp` (spawn + binary pipes) → FFI worker → `lib.call` | new | the hidden cost of the native work is the process layer, not the marshalling; `exec::Run` merges stderr into text and cannot carry a binary frame |
| **4** | pty source (`forkpty` / ConPTY), `pu run -- <cmd>` | `src/exec/run.cpp`, `app/` | the POSIX fork path already exists at `platform.cpp`; ConPTY is the part with no prior art in this tree |

Phases 0 and 1 are done, and phase 2 is half done: the store moved to `Runtime`, the
envelope was split into `page` / `buffer` / `extent`, and a cursor now carries its own
identity so a page request does not restate the command. `find` on a buffer and getting
`page` to `on_end` are what remain of phase 2 — a person reading the transcript still
cannot tell a page from a whole answer.

Phase 1 before 2 is deliberate: without elision, `find` results accumulate the same
way pages do. Phase 3 before 4 is also deliberate: the FFI worker and the pty reader
want the same thing (a child process with two binary pipes and a killable lifetime),
so build it once.

### What elision matches on, and why the store comes first

Two identities, because they answer different questions:

- **source** — the question being asked. For a command: the command and the directory
  it ran in. Two runs of `ninja` are the same source.
- **run** — the answer to it. The buffer, because each execution is its own bytes, and
  a page must be addressable without the run that produced it having to be the latest.

With both, the rules are:

1. Same source, older run → superseded by the newer run. Correct for a build, harmless
   for `date`, and always safe in the one direction that matters: the fresher answer
   stays.
2. Same source, same run, another page → **not** superseded. Pages 1..3 of one log are
   complementary, and dropping page 1 because page 3 arrived discards material the
   model may still need.
3. So a single run is bounded by a budget, not by a rule: keep the newest K pages of a
   source, turn older ones into receipts naming their line range and their buffer.

Which makes the alternative — keying on a hash of the command alone — not an
alternative at all. It is this design with the run dimension dropped and the store
removed, and its failure is the unrecoverable kind: two runs collapsed into one
identity means a page disappears with no receipt that could bring it back, which is
exactly what `tail -f` and any build that is run twice produce. The cost of the fuller
key is plumbing this tree needs regardless: `read_file`, `grep -r`, `diff` and MCP
results all want the same store, and without it each grows its own pager and its own
notion of a cursor.

One liveness rule follows, and it is easy to get wrong: an elided page must remain
resolvable, or the receipt is a dead pointer. The buffers now live as long as the
runtime does rather than as long as a tool object, which is the part that mattered;
pinning a buffer because a live node refers to it is *not* implemented, so eviction is
still by age and size alone. Today an evicted buffer makes its receipt an expired
cursor, which the tool reports instead of guessing.

Known gap, not to fix now: cursors in `session.json` do not survive a restart, so a
resumed session's receipts point at nothing. Either persist each buffer's first and
last page beside the session, or mark those receipts expired when the session loads.

## 4. Sized to this codebase, not to a wish list

- `Toolbox::GetToolDefinitions()` sends every registered tool's description and
  schema on every request. Three FFI tools (`lib_list`, `lib_describe`, `lib_call`)
  are affordable; one tool per native library is not. The progressive-disclosure
  design in `native-library-calls.md` §4 is not a preference here — it is the only
  shape the transport can carry.
- `Toolbox::RegisterTool` sanitises `[A-Za-z0-9_-]` and suffixes collisions. A
  generator that registers 400 symbols would produce `lib_foo_1`, `lib_foo_2`, and a
  catalog nobody can read. Another reason the catalog stays data, not tools.
- `SecurityPolicy` is copied into `Executor` on every rebuild and handed to tools by
  pointer. A `native` block belongs there; a parallel permission system would be the
  first thing in this tree with two sources of truth about what is allowed.
- `/agents` and `/backend` already rebuild the toolbox; anything with a lifetime
  that must outlive that rebuild (the store, the FFI worker) must live above it or it
  will be destroyed mid-session. This is the general form of B1.

## 5. What not to build now

- **A templated `Viewport<T>`.** `Slice` returns bytes and the envelope is JSON;
  a template argument would leak into the wire format. One type-erased class, small
  adapters.
- **A tool per library, or a generated tool schema from headers.** See §4: the
  transport sends every schema every turn, and `execute_bash`'s own `Description()`
  is one sentence today — the tool surface is not where this project spends words.
- **Image tiles.** `kTile` can stay in the enum so the envelope is uniform, but
  there is no image tool in this build and no consumer for a crop.
- **Attaching to a terminal this process does not own.** §5 of
  `context-viewport.md`: the screen is in the emulator, not the tty. `pu serve` has a
  browser client — a future GUI there can report its own cursor and selection
  exactly, which is cheaper and more accurate than any attach mechanism.
- **A token budget enforced by counting.** The architecture deliberately does not
  have one. Elision (phase 1) removes bytes at the only place where removal is
  provably safe; a running `max_tokens` guard would have to guess which message is
  disposable and would break the tool_call/tool_result pairing.

## 6. How to tell whether any of it worked

The data is already logged or counted:

- `ChatResult::usage.prompt_tokens` per turn (`executor.cpp:236`) — the number
  phase 1 is supposed to move. Compare a 10-page exploration before and after.
- `result.tool_call_count` — if `find` works, this should fall on search-shaped
  tasks.
- The ratio of `window` bytes to `output` bytes in stored results — a page whose
  envelope costs more than its text is over-instrumented.
- Cursor-miss refusals (`"cursor belongs to a different command"`, buffer evicted) —
  today these silently include every `/backend` switch (B1); after phase 0 they
  should only be genuine expiry.

## 7. Adjacent defects this work will brush against

Not part of the roadmap, but in the same functions and worth fixing while there:

- ~~`write_file` compares `weakly_canonical` results with a bare prefix test, so a
  sandbox of `/tmp/safe` still admitted `/tmp/safe-evil/x`.~~ Fixed: paths are compared
  by component, and the test names both shapes.
- ~~`approval_note` is declared and never assigned, so the user's words beside an
  approval were dropped.~~ Fixed: the approval's note reaches the model the way a
  refusal's does.
- `RiskLevel::kSafe`/`kNeutral` are computed and never read; `dangerous_patterns_`
  and `safe_commands_` are an implicit policy with no entry in README.
