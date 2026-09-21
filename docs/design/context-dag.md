# Context DAG Design Baseline

Frozen for stage 0 of `refactor/context-dag`. Baseline commit `f8fc99a`.

Delete this file once stage 6 lands: by then every decision lives in code or in a
commit message, and a stale copy here would only mislead.

The ten decisions below are fixed. Change this document before changing the code.
A `Stage 0 ruling` closes a point left open during review and binds as strongly as
the decision it follows.

Evidence lives in the companions; this file states decisions, not findings:

- `context-dag-inventory.md` — current usage points of `ChatMessage`, `Transcript`
  and the `Workspace` history API.
- `../ARCHITECTURE.md` (Provider Differences) — wire-format differences the
  projection must absorb.

Source references are pinned by name where possible and by line otherwise; the
lines are a reading aid from the frozen tree, not a contract. Verification
results belong to commit messages, which stay true for the commit they describe.

---

## 1. Message modelling: `std::variant` with four role payloads

**Decision.** One variant type with four alternatives, one per role.

```cpp
using MessagePayload = std::variant<UserPayload, AssistantPayload,
                                   SystemPayload, ToolPayload>;

struct MessageNode {
  MessageId id;
  MessagePayload payload;
  std::vector<MessageId> parents;
};
```

**Why.** Role is a `std::string` (`llm_provider.hpp:19`) re-tested by string
comparison at nine consumer sites. A closed variant is exhaustively checkable and
lets each payload own only the fields legal for its role.

**Consequence.** Illegal states disappear: a `user` payload has no `tool_calls`, a
`tool` payload always has a tool call id.

**Stage 0 ruling (module).** The types live in `include/pu/context/message.hpp`.

A message DAG is a domain concept, while `core/` is reserved for dependency-free
base utilities (`ARCHITECTURE.md`, Directory Structure). The type is used by both
`llm/` (projection) and `session/` (storage), so it must sit below them; a
`context/` module is that layer without redefining what `core/` means.

The two `FROZEN` comments name this path, so they move with the type.

## 2. Content representation: `std::vector<ContentPart>`

**Decision.** Message text is an ordered `std::vector<ContentPart>`, where
`ContentPart` is a variant starting with a text part.

```cpp
using ContentPart = std::variant<TextPart /*, ImagePart, ... */>;
```

**Why.** `content` is a flat string and no supported provider accepts non-text
parts, so multimodal becomes an additive variant alternative instead of a breaking
change to every consumer.

**Consequence.** Text-only paths need a flatten helper. Ordering is preserved, so
interleaved parts stay representable.

**Stage 0 ruling (encoding).** A text part holds valid UTF-8, and text from
outside pu-cli is normalised where it enters: `ExecuteCommand` for shell output,
the stdio transport for MCP servers, and `MakeToolResultJson` as the boundary
where any tool output becomes JSON. A child's encoding follows how it was
launched, so `platform` distinguishes a console child from a piped one.

## 3. Reasoning carries provider, signature, and raw JSON

**Decision.** Assistant reasoning is a struct of `provider`, `signature` and
`raw_json`, not a bare string.

**Why.** It is a plain string (`llm_provider.hpp:23`), and the OpenAI projection
echoes it verbatim (`openai_provider.cpp:43`). A provider that needs an echoed
signature or an opaque block cannot be served by a string.

**Consequence.** The projection drops reasoning where unsupported and re-emits it
intact where required. `raw_json` makes round-trips debuggable without re-parsing.

## 4. Tool calls: `ToolCallRecord` (intent) split from `ToolPayload` (receipt)

**Decision.** Intent and receipt are distinct types linked by tool call id, not by
list adjacency.

**Why.** Intent rides in `assistant.tool_calls` (`executor.cpp:297`) while the
receipt is a separate `tool` message (`executor.cpp:364`), and the only link is a
string. `Transcript::Compact` (`transcript.cpp:18`) re-derives the pairing by
scanning backwards — the strongest evidence the relationship is real and
unexpressed.

**Consequence.** Compaction, resume and interruption stop reasoning about
adjacency, and a receipt with no matching record is rejected at construction.

## 5. Tool status: `kPending`, `kRunning`, `kCompleted`, `kInterrupted`

**Decision.** Every tool call record carries one of the four states.

**Why.** `HasPendingToolCalls()` infers "in flight" from the last message's shape
(`transcript.cpp:61`), and `Session::SwitchAgent` / `SwitchBackend` block on that
inference (`session.cpp:26`, `:35`). An interrupted run leaves no trace because
there is no state to record.

**Consequence.** Status is stored, not inferred. A crash leaves a recorded
`kRunning` that the next session can surface or repair.

**Stage 0 ruling (ask_user).** `ask_user` is an ordinary tool ending in
`kCompleted`, with the reply carried by the tool payload; the executor special case
(`executor.cpp:277-283`) is deleted. Three consequences:

- `AskUserTool::Execute` (`builtin_tools.cpp:272`) is a stub returning
  `clarification_needed` and never runs today, because the executor intercepts the
  call first. It needs a real answer channel (a `ToolContext` callback, or a loop
  that pauses), otherwise the model receives a failed tool.
- `ToolLoopResult::completed` (`executor.hpp:65`) has one writer — the branch being
  deleted — and no reader. It goes with the branch.
- The `ask_user` case in `test_executor.cpp` asserts the removed behaviour and is
  rewritten in the same change.

**Stage 0 ruling (kInterrupted).** `kInterrupted` is introduced together with the
code that writes it, not before. Stage 1 defines only `kPending`, `kRunning` and
`kCompleted`, because neither cancellation path records anything today: both
`CancelToken` and `platform::IsInterrupted()` only stop the data flow.

Repeating the pattern of declaring state before its producer is what produced the
unreachable structures already in the tree: the compaction branch that no provider
can reach, `parameters_as_string` plumbed but never read, `ToolLoopResult::completed`
with no reader, and the unused token counters on `ChatResult`.

The writer lands in stage 2 or 3 for a normal cancellation, and in stage 6 for
crash recovery (load repairs `kRunning` to `kInterrupted`). Stage 1 gains the value
in whichever change first needs it.

## 6. Storage: DAG with parent references and a current leaf

**Decision.** Nodes live in a flat container keyed by stable id, each holding
parent references. Conversation position is a separate `current_leaf`. Ordered
vectors stop being the source of truth.

**Why.** `messages_` is a vector (`transcript.hpp:25`) and `Workspace::Append`
assigns `id = HistorySize() + 1` (`workspace.cpp:33`), tying identity to insertion
order and blocking rewind, branching and edit-and-retry.

**Consequence.** Ids never depend on position and are never reused. Linear replay
becomes one traversal rather than the storage format.

**Stage 0 ruling (branching).** v1 stores a single-parent chain and exposes no
fork, rewind or edit-and-retry entry point. `parents` stays a container so that
branching later is not another storage change. Out of scope for v1.

**Stage 0 ruling (concurrency).** Single-threaded, no internal lock. Writes go
through `Runtime::ProcessInput`, and the serve layer already serialises with
`io_mutex` (`serve.cpp:52`). The header states `Not thread-safe. Caller must
serialize access.`

**Stage 0 ruling (MessageId).** `std::string` holding a UUID v4, fixed now because
it also reaches clients and a position-derived integer cannot express DAG identity.

The front end needs no change: `loadHistory()` reads only `role`, `content`,
`tool_calls` and `reasoning_content`. The `id` fields in `web/app.js` belong to
tool calls and WebSocket frames. `serve_http_routes.cpp:126` is the only producer,
and no test asserts on the message id.

## 7. Request view: `BuildRequestPath(leaf, policy, caps)`

**Decision.** One function produces the message list, taking the leaf to render,
the selection policy, and the provider capabilities.

**Why.** The executor copies the whole vector on every iteration
(`executor.cpp:213`), and each provider prepends a synthetic system message when
one is absent — in four places (`ollama_provider.cpp:36`, `:74`,
`openai_provider.cpp:98`, `:127`). Selection and injection are both implicit.

**Consequence.** The executor stops copying. What the model sees becomes a pure
function of stored state plus explicit inputs, testable without a provider.

**Stage 0 ruling (system context).** The system prompt is neither a stored node nor
a `Memory` variable. It and the generated static context are explicit inputs that
the projection prepends.

This repairs a live defect. The four injection branches are dead in production:
only `config::CreateBackend` (`agent_config.cpp:194`, `:204`) copies
`system_prompt`, and it is called solely from tests. The live path,
`Session::CreateProvider` (`session.cpp:44`), is a second copy of the same switch
that drops the field. What reaches the model is the `Memory` variable written at
`session.cpp:41` and read at `executor.cpp:228`. A `SetVar` / `GetVar` audit
confirms `system_prompt` is the only `Memory` variable that reaches the request
view, so removing this channel removes the only hidden input.

Actions: delete the `SetVar` write, delete the four branches, collapse the two
switches into one, and pass both inputs as parameters.

**Stage 0 ruling (deferral).** Stage 0 records this only. The deletions ship in
stage 3, after `BuildRequestPath` lands in stage 2, so that no window is left with
no working system prompt.

## 8. Provider differences: `ProviderCapabilities` plus a visitor projection

**Decision.** Providers declare capabilities as data; projection to the wire format
is a visitor over payloads driven by those capabilities.

**Why.** The two providers differ on eleven measured dimensions, currently encoded
as duplicated request builders plus a `RoleToString` fallback that maps unknown
roles to `user` (`ollama_provider.cpp:19`). Anthropic would be a third copy of the
message loop.

**Consequence.** Adding a provider means declaring capabilities and handling what
the visitor cannot project. Gaps become explicit: the projection refuses or drops
data rather than mis-encoding it.

**Stage 0 ruling (field set).** `caps` starts at four fields:

| Field | Type |
| --- | --- |
| `echo_reasoning_content` | `bool` |
| `allows_content_with_tool_calls` | `bool` |
| `supports_multimodal` | `bool` |
| `max_context_tokens` | `std::optional<int>` |

The first three mirror projection rules that exist today: echoing reasoning,
forcing `content` to `null` when tool calls are present, and rejecting non-text
parts. The fourth is optional because the token budget is deferred, but reserving
it now avoids changing the signature twice. The set is a floor, not a ceiling.

## 9. Compaction never deletes conversation nodes

**Decision.** Compaction is a selection policy used when building a request path.
It never removes or rewrites conversation nodes; its only write is appending one
summary node.

**Why.** `Transcript::Compact` mutates `messages_` in place (`transcript.cpp:18`)
and substitutes a `"[Compressed: N messages omitted]"` system message
(`transcript.cpp:50`). The original turns become unrecoverable, and the marker is
indistinguishable from a real system turn once persisted.

**Consequence.** Compaction is non-destructive and reversible: the same store
renders under different policies, and resuming never resumes a lossy state.

**Stage 0 ruling (summary node).** The summary is materialised as a node rather
than generated while rendering, so `BuildRequestPath` stays a reader and the
summary is reusable instead of recomputed. It carries `is_synthetic = true`, which
replay, export and the UI can rely on.

**Stage 0 ruling (test authorization).** The storage-rewriting meaning of
`Transcript::Compact` is void, and the tests asserting it are replaced by tests on
the request path: `test_workspace.cpp:48` and `:103`.

## 10. Serialization: `schema_version = 2`, breaking, no back-compat

**Decision.** The session root carries `schema_version = 2` beside `workspace` and
`runtime_spec`. A missing field means the pre-DAG layout, and the file is refused.

**Why.** The old layout is a flat array at `workspace.history` with a `tool_calls`
blob (`transcript.cpp:67`), which cannot express parents, node ids, tool status or
content parts without overloading keys. Reading it would mean inventing
relationships the file never recorded.

**Consequence.** The main file is overwritten on the next save, so the stage 0
backup at `<workspace_root>/.pu/session.v1.backup.json` is the only surviving copy,
and the load failure must name it.

**Stage 0 ruling (version number).** `2` is reused, with evidence. No release wrote
a version field: `git grep -i schema` on v0.2.0 through v0.4.1 finds only the tool
`parameters_schema`, and `git log -S` for `kSchemaVersion` / `schema_version`
across `--all` finds nothing. A mention in an unreachable commit message is not a
shipped format.

One hazard remains: an unreachable branch used `2` for a different layout
(`tool_calls_json`), and a developer machine may still hold such a file. The guard
is therefore structural, not numeric — a version 2 file is accepted only with DAG
node storage and a current leaf.

**Stage 0 ruling (load failure).** Both refusals report the same shape:

```text
session.json uses the pre-DAG (v1) layout and cannot be loaded.
  file:    <workspace_root>/.pu/session.json
  reason:  missing schema_version
  backup:  <workspace_root>/.pu/session.v1.backup.json

The original conversation is preserved in the backup file only. The main file is
overwritten on the next save.
```

`reason:` is `missing schema_version` or `schema_version 2 without DAG node
storage`. The `backup:` line appears only when the backup exists. This replaces the
`spdlog::warn` in `LoadSessionFromFile` (`runtime.cpp:43`).

**Stage 0 ruling (no migration prompt).** Stage 6 reports and stops. No release
ever wrote a version field, so every existing file is the old layout and the guard
fires on all of them; a machine search found no `session.json` at all. An automatic
converter would have to read a format it cannot verify, which is what the backup
exists to avoid.

---

## Known limitations

**Token budget.** Selection is by message count (`keep_head` / `keep_tail`). A
token budget is out of scope: `ChatResult::input_tokens` and `output_tokens` are
never assigned, OpenAI logs usage at trace level only (`openai_provider.cpp:192-197`),
and Ollama does not parse it. No usage pipeline and no tokeniser dependency is
added. `ProviderCapabilities::max_context_tokens` reserves the field.

## Open questions

| # | Question | Why it matters |
| --- | --- | --- |
| 2 | How are unreachable nodes reclaimed? | Decision 9 stops compaction from deleting, so the store grows without bound. |
| 3 | When is the store persisted? | `SaveCurrentSession()` runs on input and shutdown only (`runtime.cpp:109`), so a crash loses the DAG. |

Items 1, 4, 5 and 6 were closed during the stage 0 review and their rulings are in
the decisions above; the numbers are left unshifted so earlier discussion stays
traceable.

## Verified baseline

The suite must pass under both a UTF-8 code page and a non-UTF-8 one, which is the
requirement the encoding rulings exist to satisfy. Measured results are recorded in
the commit messages that produced them.

## How later stages use this document

| Stage | Change |
| --- | --- |
| 1 | Node and payload types; tool record, payload and status; `MessageId` as UUID v4 string. Shares one UUID generator with the logging layer instead of copying it. |
| 2 | `BuildRequestPath` taking both system inputs; `ProviderCapabilities` and the projection visitor. |
| 3 | Collapse the two provider switches and delete the four dead injection branches. |
| 6 | Persist at `schema_version = 2`, delete the old reader, report with the message above. |

Stage 1 adds no caller, so its tests assert the types rather than behaviour; its
value is fixing the vocabulary that stages 2 to 6 target.

Every behaviour removal carries its test rewrite: the `ask_user` case and the two
`Compact` assertions.
