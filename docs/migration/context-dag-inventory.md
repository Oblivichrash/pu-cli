# Context DAG Migration Inventory

Baseline commit `f8fc99a`. A read-only snapshot of every place the current linear
context model is referenced. Line numbers are 1-based and will drift once stage 1
lands, so re-run the commands rather than trusting them. References into
`llm_provider.hpp` and `transcript.hpp` are taken from the stage 0 tree, where the
two `FROZEN` comments add two lines.

```text
grep -in "ChatMessage"
grep -in "Transcript|transcript_"
grep -in "GetHistory|HistorySize|ClearHistory|HasPendingToolCalls|Append\("
grep -in "history"                    # src/ and web/
```

`Kind` is one of `define`, `construct`, `read`, `serialize`, `compare`, `store`.
`Category` is `production`, `test` or `doc`.

---

## 1. `ChatMessage`

63 occurrences in 14 files: 40 production (10 files), 23 test (4 files), none in
documentation.

### 1.1 Production headers

| File | Line | Kind | Purpose |
| --- | --- | --- | --- |
| `include/pu/llm/llm_provider.hpp` | 16 | define | The aggregate itself; every context field lives here. **Frozen in stage 0.** |
| `include/pu/llm/llm_provider.hpp` | 64 | define | `LLMProvider::Chat` pure virtual — the boundary every provider implements. |
| `include/pu/llm/ollama_provider.hpp` | 31 | define | `Chat` override signature. |
| `include/pu/llm/ollama_provider.hpp` | 43, 44 | define | `BuildRequest`, `BuildRequestWithTools` signatures. |
| `include/pu/llm/openai_provider.hpp` | 34 | define | `Chat` override signature. |
| `include/pu/llm/openai_provider.hpp` | 46, 47 | define | `BuildRequest`, `BuildRequestWithTools` signatures. |
| `include/pu/session/transcript.hpp` | 15 | store | `Append`. **Frozen in stage 0.** |
| `include/pu/session/transcript.hpp` | 16 | read | `GetHistory` returns a copy of the linear vector. |
| `include/pu/session/transcript.hpp` | 25 | store | `messages_` — the sole owner of conversation state. |
| `include/pu/session/workspace.hpp` | 17 | store | `Append` forwarding API. |
| `include/pu/session/workspace.hpp` | 19 | read | `GetHistory` forwarding API. |

### 1.2 Production implementation

| File | Line | Kind | Purpose |
| --- | --- | --- | --- |
| `src/executor.cpp` | 213 | read | Copies the whole history into `chat_history` on every tool-loop iteration. |
| `src/executor.cpp` | 233 | construct | Injected system message (static context plus the `system_prompt` variable). |
| `src/executor.cpp` | 297 | construct | Assistant message carrying `tool_calls` and `reasoning_content`. |
| `src/executor.cpp` | 364 | construct | Tool result message, pairing `tool_name` and `tool_call_id`. |
| `src/llm/ollama_provider.cpp` | 27, 64 | define | Request builder signatures. |
| `src/llm/ollama_provider.cpp` | 36, 74 | compare | `std::none_of(role == "system")` decides whether to inject `system_prompt`. |
| `src/llm/ollama_provider.cpp` | 39, 77 | construct | Synthetic system message inserted at `messages.begin()`. |
| `src/llm/ollama_provider.cpp` | 184 | define | `Chat` implementation signature. |
| `src/llm/openai_provider.cpp` | 25 | serialize | `BuildMessagesJson` — the OpenAI projection: `tool_result` to `tool`, `content: null` with tool calls, `reasoning_content`, `tool_call_id`, string-encoded `arguments`. |
| `src/llm/openai_provider.cpp` | 84, 112 | define | Request builder signatures. |
| `src/llm/openai_provider.cpp` | 98, 127 | compare | System-prompt detection branch. |
| `src/llm/openai_provider.cpp` | 101, 130 | construct | Synthetic system message inserted at `messages.begin()`. |
| `src/llm/openai_provider.cpp` | 219 | define | `Chat` implementation signature. |
| `src/session/transcript.cpp` | 10 | store | `Append` pushes onto `messages_`. |
| `src/session/transcript.cpp` | 14 | read | `GetHistory` copies the vector out. |
| `src/session/transcript.cpp` | 46, 50 | construct | `Compact` buffer and the synthetic `"[Compressed: N messages omitted]"` system message. |
| `src/session/transcript.cpp` | 89 | serialize | `Deserialize` rebuilds each message field by field. |
| `src/session/workspace.cpp` | 27 | store | `Append(ChatMessage)` forwards to `transcript_`. |
| `src/session/workspace.cpp` | 32 | construct | Builds a message from `role`/`content` with a generated `id` and timestamp. |
| `src/session/workspace.cpp` | 40 | read | `GetHistory` forwards to `transcript_`. |

### 1.3 Tests

| File | Line | Kind | Purpose |
| --- | --- | --- | --- |
| `tests/unit/test_executor.cpp` | 171 | define | `MockLLM::Chat` override; the history parameter is unused. |
| `tests/unit/test_ollama_backend.cpp` | 21, 22, 59, 60 | construct | Positional aggregate init `ChatMessage{1, "now", "user", "..."}`. |
| `tests/unit/test_ollama_backend.cpp` | 98 | construct | Braced init `{{1, "now", "user", "list files"}}`. |
| `tests/unit/test_ollama_backend.cpp` | 123, 129 | construct | Tool message plus `history = {tool_msg}`, asserting `tool_name` and `tool_call_id` reach the wire. |
| `tests/unit/test_openai_backend.cpp` | 24, 25, 52, 89, 90, 121, 147, 173, 192 | construct | Request-building, streaming, error-path and thinking-mode fixtures. |
| `tests/unit/test_workspace.cpp` | 37, 51, 71 | construct | Compact fixtures built field by field. |
| `tests/unit/test_workspace.cpp` | 77, 83 | construct | Assistant message with `tool_calls` plus the paired tool response. |
| `tests/unit/test_workspace.cpp` | 117 | construct | Round-trip fixture for `Serialize` / `Deserialize`. |

Positional aggregate initialisation appears at five call sites, so adding or
reordering fields silently changes meaning.

---

## 2. `Transcript`

38 occurrences in 8 files: 21 production, 9 test, 8 doc.

### 2.1 Production

| File | Line | Kind | Purpose |
| --- | --- | --- | --- |
| `CMakeLists.txt` | 38 | define | `src/session/transcript.cpp` in `pu_core`. |
| `include/pu/session/transcript.hpp` | 13 | define | Class declaration. **Frozen in stage 0.** |
| `include/pu/session/transcript.hpp` | 22 | serialize | `static Transcript Deserialize`. |
| `include/pu/session/workspace.hpp` | 8 | define | Include. |
| `include/pu/session/workspace.hpp` | 37 | store | `transcript_` member — the sole context store. |
| `src/session/transcript.cpp` | 2 | define | Self include. |
| `src/session/transcript.cpp` | 10 | store | `Append`. |
| `src/session/transcript.cpp` | 14 | read | `GetHistory`. |
| `src/session/transcript.cpp` | 18 | read | `Compact` mutates the stored vector. |
| `src/session/transcript.cpp` | 61 | read | `HasPendingToolCalls` inspects `messages_.back()`. |
| `src/session/transcript.cpp` | 67 | serialize | `Serialize` to a JSON array. |
| `src/session/transcript.cpp` | 85, 86 | serialize | `Deserialize` entry point and constructed result. |
| `src/session/workspace.cpp` | 28, 41, 45, 49, 53 | read | Read and write forwarding to `transcript_`. |
| `src/session/workspace.cpp` | 79 | serialize | `Serialize` writes the `history` key. |
| `src/session/workspace.cpp` | 91 | serialize | `Deserialize` reads the `history` key. |
| `src/session/workspace.cpp` | 105 | store | `ClearHistory` resets `transcript_`. |

### 2.2 Tests

| File | Line | Kind | Purpose |
| --- | --- | --- | --- |
| `tests/unit/test_workspace.cpp` | 34, 35 | construct | Compact no-op under threshold. |
| `tests/unit/test_workspace.cpp` | 48, 49 | construct | Compact keeps head + summary + tail. |
| `tests/unit/test_workspace.cpp` | 68, 69 | construct | Compact preserves tool-call pairing. |
| `tests/unit/test_workspace.cpp` | 115, 116, 124 | construct, serialize | Round-trips tool calls as a JSON array. |

### 2.3 Documentation

| File | Line | Purpose |
| --- | --- | --- |
| `docs/ARCHITECTURE.md` | 38 | `Workspace` container table naming `Transcript`. |
| `docs/ARCHITECTURE.md` | 93 | Full tool JSON is never persisted in the transcript. |
| `docs/ARCHITECTURE.md` | 233, 236 | Executor flow: read transcript, store result into transcript. |
| `docs/ARCHITECTURE.md` | 295, 297 | Compaction section describing `Transcript::Compact`. |
| `docs/ARCHITECTURE.md` | 343 | Source tree listing `session/`. |
| `README.md` | 177 | Transcript stores extracted stdout/error. |

---

## 3. `Workspace` history API

52 call sites in 11 files. Three further `append(` hits are `std::string::append`
and unrelated: `core/logging.cpp:108`, `llm/streaming_json_parser.cpp:10`,
`mcp/http_transport.cpp:66`.

### 3.1 Declarations

| File | Line | Symbol |
| --- | --- | --- |
| `include/pu/session/workspace.hpp` | 17, 18 | `Append(const ChatMessage&)`, `Append(role, content)` |
| `include/pu/session/workspace.hpp` | 19, 20 | `GetHistory`, `HistorySize` |
| `include/pu/session/workspace.hpp` | 21, 22 | `Compact`, `HasPendingToolCalls` |
| `include/pu/session/workspace.hpp` | 24 | `ClearHistory` |
| `include/pu/session/session.hpp` | 55 | `Session::HasPendingToolCalls` inline forwarding |

### 3.2 Writes

| File | Line | Purpose |
| --- | --- | --- |
| `src/session/workspace.cpp` | 27, 28, 31, 33, 37 | Append implementation, id and timestamp assignment. |
| `src/session/workspace.cpp` | 104 | `ClearHistory` resets the store. |
| `src/executor.cpp` | 148 | Appends the user turn before the tool loop. |
| `src/executor.cpp` | 170 | Appends the final assistant response. |
| `src/executor.cpp` | 276 | Appends the assistant error message on failure. |
| `src/executor.cpp` | 315 | Appends the assistant message carrying `tool_calls`. |
| `src/executor.cpp` | 369 | Appends each tool result. |
| `src/app/serve_http_routes.cpp` | 218 | `/api/clear` clears history and artifacts. |
| `src/command_router.cpp` | 180 | `/clear`. |

### 3.3 Reads

| File | Line | Purpose |
| --- | --- | --- |
| `src/executor.cpp` | 214 | Snapshots history for the provider request. |
| `src/app/serve_http_routes.cpp` | 120 | `/api/history` serialises the linear list. |
| `src/session/session.cpp` | 26, 35 | Blocks agent and backend switching while tool calls are pending. |
| `src/session/workspace.cpp` | 40, 41, 44, 52, 53 | Read forwarding. |
| `tests/unit/test_executor.cpp` | 308 | Asserts the tool message is paired in workspace history. |
| `tests/unit/test_workspace.cpp` | 11, 12, 14, 15, 41, 55, 58, 75, 82, 87, 92, 106, 109, 122, 125, 129 | Workspace and Transcript behaviour tests. |

### 3.4 Outside C++

| File | Line | Purpose |
| --- | --- | --- |
| `web/app.js` | 347–373 | `loadHistory()` consumes `/api/history`: `role`, `tool_calls`, `reasoning_content`. |

---

## 4. Summary

| Object | Occurrences | Files | Production / test / doc |
| --- | --- | --- | --- |
| `ChatMessage` | 63 | 14 | 40 / 23 / 0 |
| `Transcript` | 38 | 8 | 21 / 9 / 8 |
| Workspace history call sites | 52 | 11 | — |

| Category | Files | Functions or methods |
| --- | --- | --- |
| Production C++ | 14 | ~30 |
| Tests | 4 | ~13 |
| Docs and web UI | 3 | ~4 call sites |
| **Total** | **21** | **~47** |

Production files: `llm_provider.hpp`, `ollama_provider.{hpp,cpp}`,
`openai_provider.{hpp,cpp}`, `transcript.{hpp,cpp}`, `workspace.{hpp,cpp}`,
`session.hpp`, `executor.cpp`, `serve_http_routes.cpp`, `command_router.cpp`,
`CMakeLists.txt`. Tests: `test_workspace.cpp`, `test_executor.cpp`,
`test_ollama_backend.cpp`, `test_openai_backend.cpp`. Docs and UI:
`ARCHITECTURE.md`, `README.md`, `web/app.js`.
