# Naming and structure cleanup

Status: plan, agreed in outline, nothing renamed yet. Every decision below is settled;
Batch 0 is ready to start and depends on none of the renames.

The project has changed shape several times — a third backend type, a turn that now
ends with its reader, a workspace-switch endpoint removed, the ACP route abandoned —
and a few names have stopped describing what they hold. This is the inventory of those,
and the order to fix them in. It is not a rewrite: the rules already in
`ARCHITECTURE.md` (where a header lives, what each target is) still hold, and moving
files that follow them would be churn rather than clarity.

## What was decided

- **Names may change inside and out.** `agents.json` fields, REST paths and the keys
  inside `session.json` may be renamed, with the docs and the front-end changed with
  them: a name that lies costs more than a file that needs editing.
- **Nothing is kept for the sake of an older file.** This is a prototype, so there is no
  migration, no backup copy, and no reading of a field a previous build wrote. A store
  this build cannot read is refused, the reason is reported, and the next save replaces
  it.
- **One concept, one word.** `workspace` stays the word for the directory, which is what
  it already means to a user, and the stored conversation stops borrowing it. This is
  the opposite of the first instinct — renaming the directory side — and the reason is
  in [Which side is renamed](#which-side-is-renamed).
- **Nothing dead stays.** A method with no caller is deleted, including one that only a
  test calls as a fixture: the test is rewritten to say what it meant.
- **Comments that explain why stay.** A note like "what `on` meant before there was a
  level" is what stops the same argument coming back; it is not residue.

## Names that lie

| Where | What it holds |
| --- | --- |
| `include/pu/agent.hpp` | the whole configuration model — `BackendType` (:19), `SecurityPolicy` (:21), `BackendConfig` (:27), `AgentEntry` (:98), `AgentsConfig` (:106), `ServeOptions` (:115), the loaders — **and** `AgentManager` (:136). Named for one class it contains, while `ARCHITECTURE.md:388` sends readers here for backends |
| `include/pu/session/session.hpp` | `Transcript` (:21), `Workspace` (:47), `RuntimeSpec` (:74), `Session` (:113). `RuntimeSpec` is session state under the runtime's name; `Workspace` is the stored conversation while `workspace_root_` (`runtime.hpp:90`) is the directory |
| `include/pu/executor.hpp` | `ExecutionResult` (:31) and, privately nested, `ToolLoopResult` (:74) — the same turn outcome twice, the second with `final_response` where the first has `content` |
| `docs/ARCHITECTURE.md` | speaks of a "single-session" server where the rule is now one directory, one process, one page |

## Dead, or alive only in tests

| Where | Callers |
| --- | --- |
| `Runtime::GetWorkspaceName()` (`runtime.hpp:43`) | none; orphaned when `/api/workspace/switch` was removed |
| `StdioTransport::IsRunning()` (`mcp/stdio_transport.hpp:32`) | none |
| `Runtime::SwitchWorkspace()` (`runtime.hpp:40`) | `tests/unit/test_backend_source.cpp` only |
| `Workspace::HistorySize()` (`session/session.hpp:54`) | tests only, while production uses `GetHistory().size()` |
| `Executor::GetStaticEnvInfo()` (`executor.hpp:70`) | `tests/unit/test_executor.cpp` only |
| `StdioTransport::ReaderLoop()` on Windows (`src/mcp/stdio_transport.cpp:253`) | an empty stub; the loop that runs is `ReaderThreadProc` |

## Compatibility with nothing left to be compatible with

| Where | What remains |
| --- | --- |
| `llm/llm_provider.hpp:61` | `enable_thinking`, a boolean still read as a thinking level |
| `session/session.cpp:36`, `llm/projection.cpp:18` | the removed `tool_result` role, still remapped to `tool` |
| `src/runtime.cpp:78,129` | `BackupLegacySession`, for a layout that no longer exists |
| `tests/unit/test_session_schema.cpp:19`, `tests/unit/test_command_router.cpp:96` | fixtures named for what was removed (`LegacySession`, "rejects removed /note command") |

## Two or three of everything

| What | Where |
| --- | --- |
| whitespace trim, and collapsing a message to one line | `core/beast_http_client.cpp:130,239`, `command_router.cpp:81`, `openai_provider.cpp:239` |
| summarising a provider's error body | `beast_http_client.cpp:213`, `openai_provider.cpp:28`, `ollama_provider.cpp:78` |
| UTF-8 continuation-byte logic | `core/text.hpp:23` and `llm/streaming_json_parser.cpp:25` |
| reading a string field defensively | `SafeString` (`openai_provider.cpp:19`) beside `json::ValueOrDefault<std::string>` |
| the REST result envelope | nine hand-built `{"ok"/"success", ...}` objects in `serve_http_routes.cpp` |
| a test helper | `ScopedEnvVar` in `tests/mocks/test_helpers.hpp` and again in `tests/unit/test_boost_integration.cpp` |
| provider capability tables | `openai_provider.cpp:38`, `ollama_provider.cpp:20` |

## Target names

| Today | Becomes | Why |
| --- | --- | --- |
| `include/pu/agent.hpp` | `pu/config/agents.hpp` (model, loaders), `pu/config/backend.hpp` (`BackendType`, `BackendConfig`, `CreateBackend`), `pu/agent_manager.hpp` (`AgentManager`) | three things are currently named after one file |
| `RuntimeSpec` | `SessionSpec` | it describes a session |
| `session.json` keys `workspace.history`, `runtime_spec` | `conversation.history`, `session_spec` | renamed with the types; an older file is refused rather than converted |
| `Workspace` (the class) | `Conversation` | the directory keeps the word |
| `Transcript` | merged into `Conversation` | a thin wrapper over `MessageGraph` with nothing else between them |
| `workspace_root_`, `GetWorkspaceRoot()` | unchanged | the directory is what that word means |
| `ToolLoopResult` | deleted; the loop fills `ExecutionResult` | one turn, one result |
| `pu_core` (target) | `pu_lib` | it holds the domain modules too, which `ARCHITECTURE.md:380` already admits |
| `GetWorkspaceName`, `IsRunning`, `SwitchWorkspace`, `HistorySize`, `GetStaticEnvInfo` | deleted, tests rewritten | nothing calls them |

## Which side is renamed

`workspace` already means the directory everywhere a user meets the word:
`/api/workspaces` lists directories, a session *is* a directory, and a server serves the
one it was started in. Renaming the directory side — the first instinct — would leave
the API saying one thing and the code another. Renaming the stored conversation instead
costs nothing on the outside and gives each layer one word:

```
workspace (a directory)  →  Session (a Conversation + its SessionSpec)  →  MessageGraph
```

## The store is not kept

The keys inside `session.json` (`workspace.history`, `runtime_spec`) are renamed with the
types, and a file this build cannot read is refused rather than converted or copied
aside: the reason is reported and the next save replaces it. That also removes the copy
taken before every load today (`BackupLegacySession`, `src/runtime.cpp:78`, whose report
sits at `:59-69`) — with nothing kept for an older layout, keeping a copy of one is a
promise the prototype does not make.

## Batches

Each batch ends green — clang-format gate, build, tests under both code pages — and is
worth a commit of its own.

**Batch 0 — delete what nothing calls, collapse the duplicates.** No renames. Delete
`GetWorkspaceName`, `IsRunning` and the Windows `ReaderLoop` stub; fold `ToolLoopResult`
into `ExecutionResult`; move trim to `text::`, the error summary to one home, the UTF-8
test to `text::`, the REST envelope to two helpers, and `SafeString` to
`json::ValueOrDefault`; drop the three compatibility paths and the backup copy taken
before every load (`BackupLegacySession`, its report, and the paragraphs in `README.md`
and `ARCHITECTURE.md` that document `session.backup.json`). Rewrite the tests that used
the deleted methods to say what they meant (`Runtime` built at the target directory,
`GetHistory().size()`).

**Batch 1 — split the configuration header.** `agent.hpp` into `config/agents.hpp`,
`config/backend.hpp` and `agent_manager.hpp`; update includes, and the extension points
in `ARCHITECTURE.md`. Pure movement.

**Batch 2 — one word per concept.** `Workspace` → `Conversation` with `Transcript`
merged into it, `RuntimeSpec` → `SessionSpec`, and the store's keys with them — an older
file is refused, not converted. Docs and the `/api/*` copy that says "session" where it
means a conversation move with it.

**Batch 3 — targets, and names on the wire.** `pu_core` → `pu_lib`; then re-read
`/api/*` and the `agents.json` fields against the vocabulary Batch 2 settled on. Most
already read well (`default_agent`, `agents`, `backend`, `security`, `serve`); the
candidate is `/api/workspaces`, which lists directories, not workspaces.

## What is not proposed

- Moving files whose placement follows `ARCHITECTURE.md:378` — `src/app/serve_internal.hpp`
  and `src/mcp/http_transport.hpp` among them.
- Rewriting comments that explain why a thing is not done another way.
- Deleting the design docs for routes not taken (`multi-session-serve.md`, and the ACP
  note at `codebuddy-cloud-api.md:8`): keeping them under a status line is how this
  repository has chosen to stop the same discussion happening twice.
- Splitting `src/app/` per surface. It is five files, and they read top to bottom.
