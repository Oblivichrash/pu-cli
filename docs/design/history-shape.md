# One chain, three budgets, one ladder

The question this answers: should the transcript be split into two lanes — tool calls
and replies — compressed by different rules, with old tool calls dropped outright? And
since a tool result is *the next turn's input*, can a middle layer rewrite it before the
model sees it? And where does `grep` sit relative to that middle layer?

**Conclusions up front:**

1. **Two message sequences are not available.** The wire format has referential
   integrity; a tool result cannot be dropped without dropping its call. What *can* be
   split is not structure but *budget* — and the budget that is actually unbounded today
   is neither of the two the question names.
2. **A middle layer is right, but it must be a projection with a pointer, not a rewrite
   per turn.** Compression that cannot be undone is not compression, it is data loss
   with a nice name.
3. **`grep` is the lower bound of that middle layer, not a rival.** It is a predicate
   projection with the predicate supplied by the consumer. Any automatic summariser has
   to be shown to beat "let the model ask again, differently" before it earns a
   default-on.

---

## 1. Why two lanes is not on the table

`BuildRequestPath` emits OpenAI-shaped messages. Two invariants hold on that shape:

- every `assistant.tool_calls[i].id` must be answered by a `tool` message carrying the
  same `tool_call_id`, and the answer must follow the call;
- a `tool` message whose `tool_call_id` names no call is rejected or silently dropped by
  most providers.

So the smallest legal unit of removal is the **pair** `(assistant tool_calls, tool
result)`. You can replace the pair with a stub; you cannot delete half of it. This is
not a vendor quirk to be worked around — it is what makes the transcript a transcript
rather than two logs interleaved.

There is a second, softer invariant: **projection must be monotone**. Prompt caching
matches the longest stable prefix, so editing a node at position *i* invalidates
everything after *i*. A rule that elides a node and later un-elides it — or a summariser
that produces different text on two runs of the same chain — turns every turn into a
cache miss and makes a transcript unreproducible. Elision decisions may only move
*down* the retention ladder, never back up.

Both invariants point the same way: **store one chain, project it under several
policies.** The code already has that split — `MessageGraph` is storage,
`BuildRequestPath` is projection, and `superseded_by` is a stub-in-place rather than a
deletion. The design work is extending the policy set, not restructuring the graph.

## 2. The lanes that do exist: three budgets

The question assumes two lanes (calls, replies). Measuring the current transcript
against real sessions, the growth rates are not two:

| Pool | Contents | Grows with | Bounded today? |
|---|---|---|---|
| prose | user text, assistant `content`, `reasoning` | turns | no, but physically slow |
| observations | `ToolPayload::content` | bytes read | **yes** — page cap + elision |
| scaffolding | `tool_calls[].id/.name/.arguments` | number of calls | **no** |

Scaffolding is the one nobody budgets for, and it is the strongest argument for the
instinct behind the question. Two multipliers:

- **Argument bodies are never elided.** `RenderMessage` serialises
  `record.arguments` in full, and the graph stores it forever. Twenty `write_file`
  calls of 200 lines each means 4000 lines of file content re-sent every turn, forever,
  on the *input* side of the tool call. The elision added for results does not touch it.
- **Ids and names are pure overhead.** A call costs roughly 50–80 tokens of
  `id`/`name`/punctuation before its arguments. At 200 calls that is ~12k tokens of
  protocol that carries no information at all, and no rule currently removes it.

So the split worth making is **prose / observations / scaffolding**, with a separate
budget each. Observations saturate first and recover cheapest. Scaffolding is the only
pool where the honest answer really is "drop old ones", because below the tombstone
threshold it contributes nothing a model can use.

`reasoning` deserves a note: it belongs to prose but is often the largest part of it,
and on providers that bind a signature to it, it cannot be re-summarised without
invalidating the signature. Treat it as write-once, droppable-only-as-a-whole.

## 3. The ladder

Four states, monotone, per node:

| Tier | What survives | Enters when | Cost |
|---|---|---|---|
| T0 live | the whole text | a run is the newest for its source, or a page was read deliberately after a newer run | full |
| T1 receipt | verdict, `window`, cursor, `superseded_by` | a newer run of the same source has been read | ~40 tokens |
| T2 tombstone | `command`+`cwd`+`exit`+line count+buffer id, no cursor | the pool is over budget and the pair is older than the newest K | ~15 tokens |
| T3 gone | nothing | scaffolding pool is over budget | 0 |

T0/T1 already exist. T2 and T3 are the new work, and they differ in an important way:
T1 is *derived* (it states a fact about another node), T2 is *remembered* (it is the only
remaining trace that something happened). T2 is worth its 15 tokens because what
prevents a model from repeating a failed approach is knowing that it tried — and
`argv + exit` is the durable half of that, while the output body is the volatile half.
**Compress along the body axis; never along the argv/verdict axis.**

T3 must be applied to whole pairs, oldest first, keeping the newest K pairs intact
(K ≈ 6 covers a plan-and-verify cycle). Sequence per turn: T3 down to K, then T2 until
observations fit, then T1 as sources are superseded naturally.

One thing T3 must not do: silently renumber or re-point a cursor. A T2 tombstone whose
buffer has been evicted from the pager keeps the id and says `evicted`, exactly as a
stale cursor is refused rather than resolved to a neighbour today.

## 4. The middle layer, at two levels

There are two places to reduce a tool result, and they are already different code:

- **ingest-time** — the pager, when the tool returns. Cost paid once, by the producer,
  deterministically.
- **projection-time** — elision, when the request is built. Costs nothing per turn
  because it is a substitution over already-stored text.

The layer the question asks for is a third: **semantic rewriting** (summarise, extract,
restructure) between the tool and the model. It is legitimate, under three conditions:

1. **It must be reversible through a pointer.** The original stays in the pager and the
   rewritten text carries the cursor that fetches it. Otherwise the model has been given
   an opinion it cannot check — the same failure recorded in `context-viewport.md` §9.
2. **It must be labelled.** The envelope says `derived: {kind: "summary", of: "o17"}`.
   An unlabelled summary gets quoted back as if it were output, and the model's later
   reasoning is then built on text it believes is verbatim.
3. **It must be computed once and stored.** Generating it per turn makes the transcript
   non-deterministic: two replays of one session differ, regression tests can no longer
   assert on a request body, and the stable prefix a prompt cache needs is different
   every time.

The cost is easy to underestimate: a summariser call per tool call doubles round-trip
latency for cheap commands. That is why this belongs behind a setting, off by default,
and only for nodes already cool (not the newest turn, whose text is the thing the model
is about to act on).

A cheaper form of the same idea, available today: **structural rewriting**. `grep`'s
`path:line:` rows, a compiler's diagnostics, a diff's hunks — these can be reduced to
counts plus ranges without any model call. `window.marks` is that reduction. It is the
80% of "transform the output before the model sees it" that costs nothing and cannot
hallucinate.

## 5. Where grep sits

`grep` and the summariser are the same operation with different owners of the predicate:

| | predicate chosen by | cost | repeatable | result self-explaining |
|---|---|---|---|---|
| `grep` / `find` | the model | 0 (just IO) | yes, idempotent | yes |
| summariser | the middleware | one LLM call | no | no — needs the label |

So `grep` is not an alternative to the middle layer; it is **its lower bound**. Any
middleware that rewrites output has to be better than the model re-asking with a
different predicate, and re-asking is free.

But today's `grep` is structurally weaker than that table suggests, because it is a
**shell command**: using it means *executing again*, not *projecting over what is
already retained*. Three consequences, all avoidable:

- it re-runs `make`, a test suite, a slow query — the expensive thing being searched;
- its output can differ from the page the model already has (nondeterminism, and a
  second version of the same truth in the transcript);
- the page already in the pager is not searched at all.

The fix is `find` over a retained source (`context-viewport.md` §4 and §8, stage 2):
`o17 ? "error"` returns `[line, snippet]` pairs from the buffer, not from the disk. Then
paging and searching become two access modes over one source —

- positional: `o17:400` (the pager)
- predicate: `o17 ? "error"` (find)

— sharing one cursor space and one budget, with neither re-running anything. That is the
same relationship `less` has to `/` in `less`, and it is why a pager without search is a
pager a person would refuse to use.

Two caveats specific to grep results: they are **rows, not lines** (`path:line:text`),
so a noisy search needs its own truncation ranked by relevance, not a raw line page; and
a search that matches 3000 times must be able to answer "3000 matches, here are 20" —
which is a count-plus-range envelope, not a page.

## 6. Order of work

| Step | Change | Why first |
|---|---|---|
| P0 | `ToolPayload` carries a parsed `PageOrigin` field; `BuildRequestPath` stops parsing JSON | today every turn re-parses every historical tool result to decide elision — O(history bytes) of parsing per turn |
| P1 | three pools + T2 tombstone in projection | bounds scaffolding and argument bodies, the two unbounded pools |
| P2 | `find` over a retained buffer | makes search a projection instead of a re-execution |
| P3 | large `arguments` stored by reference into the pager | `write_file` bodies are the largest single unelided payload |
| P4 | optional, cached, labelled summariser | needs P1–P3 to be useful and safe |

P0 is small and measurable: it removes a per-turn cost that grows with the session and
also makes the elision rules testable without a JSON round trip.

## 7. Routes not taken

- **Two message sequences.** Protocol rejects orphan tool messages; the merge point
  would have to re-serialise both lanes into one request anyway, which is the projection
  layer, one indirection later.
- **Drop tool results, keep `tool_calls`.** Produces an invalid request on every
  provider, and on the ones that tolerate it, a request that claims a call was made and
  never answered.
- **Summarise on every turn.** Nondeterministic, latency-doubling, cache-hostile, and
  untestable.
- **Elide by budget only, with no tier distinction.** Loses the difference between "this
  was superseded, ask for it back by cursor" and "this happened, the text is gone" — and
  those two need different model behaviour.

## 8. Open questions

1. Is K (newest pairs kept whole) fixed, or derived from the remaining pool balance? A
   fixed K is predictable; a derived one adapts to a small model.
2. When a T2 tombstone's buffer has been evicted, keep the id as `evicted` or drop it?
   Keeping it lets the model know the difference between "never read" and "read and
   lost", at the cost of a dangling pointer in the transcript.
3. Should `reasoning` be eligible for T2 at all, given that some providers bind a
   signature to the block and a rewritten block fails verification?
4. Does the scaffold pool need a per-turn cap as well as a total cap? Parallel tool calls
   can add fifty calls in one turn.
