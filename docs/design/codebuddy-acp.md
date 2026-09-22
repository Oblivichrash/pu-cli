# CodeBuddy ACP Integration Requirements

Status: plan for work not yet started. Delete this file once the feature is stable
and its behaviour is described in `ARCHITECTURE.md`.

Audience: the implementing agent. This document states scope, constraints and
acceptance. It does not discuss alternatives except where a decision is still open.

Everything below marked **verified** was checked against the ACP specification at
`agentclientprotocol.com` and against this repository. Everything marked **to
discover** is CodeBuddy-specific and must be answered by phase 1 before dependent
code is written.

---

## 1. Goal

Add a native CodeBuddy (China region) backend, so an `agents.json` entry with
`"type": "codebuddy"` drives CodeBuddy's full agent capabilities - tool use, file
operations, terminal execution, multi-turn sessions - without an external proxy
process.

---

## 2. Verified facts

### 2.1 The ACP specification

ACP is a JSON-RPC 2.0 protocol and is **transport-agnostic**. CodeBuddy binds it
to SSE over HTTP; that binding is a CodeBuddy detail and is not part of the spec.

| Fact | Detail |
| --- | --- |
| Method names | `initialize`, `authenticate`, `session/new`, `session/load`, `session/prompt`, `session/set_mode`, `logout` |
| Notification from client | `session/cancel` |
| Notification from agent | `session/update` |
| Version field | `result.protocolVersion`, a single **integer** MAJOR version |
| Implementation info | `result.agentInfo` (three fields: `name`, `title`, `version`) and `clientInfo` in the request |
| Cancellation | The client sends `session/cancel` as a **notification**, and the turn ends with `stopReason: "cancelled"` |
| Stop reasons | `end_turn`, `max_tokens`, `max_turn_requests`, `refusal`, `cancelled` |
| Direction | **Bidirectional.** The client exposes methods too (below) |
| Paths | All file paths in the protocol **must** be absolute; line numbers are 1-based |
| Extensibility | Custom data goes in `_meta`; custom methods are prefixed with `_` |

### 2.2 The client is a server too

An implementation that only sends requests is incomplete. The agent calls back
into the client:

| Method | Direction | Requirement |
| --- | --- | --- |
| `session/request_permission` | agent to client | **Baseline.** Every client must implement it |
| `fs/read_text_file` | agent to client | Optional, advertised by `clientCapabilities.fs.readTextFile` |
| `fs/write_text_file` | agent to client | Optional, advertised by `clientCapabilities.fs.writeTextFile` |
| `terminal/create`, `terminal/output`, `terminal/wait_for_exit`, `terminal/kill`, `terminal/release` | agent to client | Optional, advertised by `clientCapabilities.terminal` |
| `elicitation/create` | agent to client | Optional, advertised by `clientCapabilities.elicitation` |

Two consequences follow, and both are load-bearing:

- A client that never answers `session/request_permission` leaves a tool that
  needs authorisation **hanging forever**.
- `clientCapabilities` is a promise: whatever is advertised must work.

The specification also states that on cancellation the client **must** answer all
pending `session/request_permission` requests with the `cancelled` outcome, and
**should** still accept tool call updates that arrive after `session/cancel`.

### 2.3 `session/update` variants

The agent reports progress through `session/update` notifications. The variant
list below is from the specification; **the exact set CodeBuddy emits must come
from phase 1's captured stream**, not from this table.

| `sessionUpdate` | Carries |
| --- | --- |
| `agent_message_chunk` | `content` (a `ContentBlock`), optional `messageId` |
| `agent_thought_chunk` | reasoning text |
| `user_message_chunk` | the echoed user message |
| `tool_call` | `toolCallId`, `title`, `kind`, `status: "pending"` |
| `tool_call_update` | `toolCallId`, `status` (`in_progress`, `completed`, ...), optional `content` |
| `plan` | `entries[]` with `content`, `priority`, `status` |
| `usage_update` | `used`, `size` token counts, optional `cost` |
| `available_commands_update` | slash commands (CodeBuddy extension) |
| `current_mode_update` | session mode changes |

Chunks sharing a `messageId` belong to one message; a changed `messageId` starts a
new one.

### 2.4 This repository

| Fact | Detail |
| --- | --- |
| Test count | **167**, and both a UTF-8 and a non-UTF-8 console must pass |
| `ExecutionResult` | Has `content`, `was_streamed`, `has_error`, `error_message`, `tool_call_count`. **No cancellation flag** |
| `ToolCallbacks` | Already exists in `executor.hpp`, with `on_start(id, name, args)` and `on_end(id, output, error)` |
| Streaming | `llm::StreamingJsonParser` is a line splitter with partial-UTF-8 handling; reusable for SSE framing |
| HTTP | `BeastHttpClient::PostStream` streams a response body to a callback, and reports a 4xx/5xx body through the thrown `HttpError` |
| Directory roles | `core/` base utilities, then `context/`, `infra/`, `llm/`, `mcp/`, `session/`, `tools/` domain modules, with orchestration headers at the `include/pu/` root |
| `mcp/` | Defined by `ARCHITECTURE.md` as the MCP client stack; an ACP client is not MCP |
| Frozen | `ChatMessage` is frozen for new fields; `Transcript`'s public surface is frozen. Neither needs changing here |

---

## 3. Architecture decisions

### D1 (root decision): where tools execute

CodeBuddy may execute tools on the server, or call back into the client through
`fs/*` and `terminal/*`. This is a product decision, not a discovery, because it
determines what `clientCapabilities` advertises and therefore what the client must
implement.

| Option | `clientCapabilities` | Consequence |
| --- | --- | --- |
| Server-side execution | `{}` (only the baseline `session/request_permission`) | Smaller surface, safer. CodeBuddy cannot touch the local workspace |
| Client-side execution | `fs.readTextFile`, `fs.writeTextFile`, `terminal`, as chosen | Reuses the existing `SecurityPolicy` for sandboxing, but the client must implement every method it advertises, with the protocol's absolute-path rules |

**Phase 1 must resolve this**, because the transport and interface shapes below
depend on it. Advertising nothing is the smaller first step and can be widened
later; that is the recommended default unless the local-filesystem behaviour is
required from the start.

### D2: SSE binding form

The specification does not define how JSON-RPC messages map onto HTTP requests.
Two shapes are plausible and they need different routing:

| Shape | Description | What it needs |
| --- | --- | --- |
| Request-scoped | One POST carries one JSON-RPC message; its SSE stream carries the reply plus any notifications and inbound requests that occur while it is open | A way to reply to an inbound request while the stream is open (a second POST, or a stream write) |
| Session-scoped | One long-lived POST carries the whole session in both directions | A writer on the open request body; not supported by `PostStream` as it stands |

**Phase 1 must determine which one CodeBuddy implements.** If it is session-scoped,
`BeastHttpClient` needs an extension and that becomes its own task before phase 2.

### D3: settled decisions

| Decision | Content |
| --- | --- |
| Entry point | Not through `LLMProvider::Chat()`. `Runtime::ProcessInput` gains a parallel ACP branch |
| Transport | Reuse `BeastHttpClient::PostStream`, and reuse the line splitting in `llm::StreamingJsonParser` **after** the two adjustments in D4 |
| Protocol layer | A new `AcpRpcClient`, separate from the MCP `JsonRpcClient` |
| Session layer | A new `AcpAgentClient` owning `initialize` / `session/new` / `session/prompt` |
| Config | Add `BackendType::kCodeBuddy`, recognising `"type": "codebuddy"` |
| Tools | CodeBuddy manages tools; pu-cli registers no `Toolbox` on this path |
| Directory | New `include/pu/acp/` and `src/acp/`. Not `mcp/`, which has a defined meaning, and not a new `agent/` beside the existing root-level `agent_config.hpp` |
| Callbacks | Reuse `ToolCallbacks` and `ProcessInput`'s existing `content_callback`. Define no second callback type |
| Feature switch | `PU_ENABLE_CODEBUDDY=1` gates the backend until it is stable |

### D4: what to do about the existing line parser

`llm::StreamingJsonParser` splits a byte stream into lines and holds back a
partial UTF-8 sequence until the next chunk, which is what SSE framing needs. It
has two problems as it stands.

**It discards blank lines.** `Feed` skips an empty line without reporting it:

```cpp
if (line.empty()) {
  buffer_.erase(0, pos + 1);
  continue;            // the signal SSE uses to end an event
}
```

SSE delimits an event with a blank line, and a `data:` field may repeat, so the
blank line is how a multi-line payload is known to be complete. A client that
dispatches on each `data:` line instead will mishandle a payload split across
several of them. The parser must either report blank lines or SSE framing must
live in the transport.

**It lives in `llm/`.** It knows nothing about language models; it is a framing
utility. Importing it from `acp/` would make a domain module depend on a peer
module for a utility, which the repository's layering does not otherwise do.
Moving it to `core/` - as `uuid` and the message graph were moved during the
context refactor - removes both problems.

Phase 2 resolves this. The options are to extend the parser and move it to
`core/`, or to write SSE framing in `AcpTransport` and not reuse the class.

---

## 4. Files

### 4.1 New

| File | Responsibility |
| --- | --- |
| `include/pu/acp/codebuddy_binding.hpp` | The CodeBuddy-specific layer: environment variables, the required `X-CodeBuddy-Request` header, the endpoint path, the ACP version supported. Header-only |
| `include/pu/acp/acp_transport.hpp`, `src/acp/acp_transport.cpp` | HTTP POST plus SSE ingest, header attachment, cancellation |
| `include/pu/acp/acp_rpc_client.hpp`, `src/acp/acp_rpc_client.cpp` | JSON-RPC 2.0 routing across all four message categories |
| `include/pu/acp/acp_agent_client.hpp`, `src/acp/acp_agent_client.cpp` | `Initialize`, `Run`, `Cancel`; version negotiation; capability-driven parsing |
| `tests/unit/test_codebuddy_binding.cpp` | Environment and header construction |
| `tests/unit/test_acp_rpc_client.cpp` | Routing, promise resolution and rejection |
| `tests/unit/test_acp_transport.cpp` | Headers, body, cancellation, SSE framing with a mock client |

### 4.2 Modified

| File | Change |
| --- | --- |
| `include/pu/agent_config.hpp` | `BackendType::kCodeBuddy` |
| `src/agent_config.cpp` | Parse `"codebuddy"`; the feature switch is enforced where 6.3 states |
| `include/pu/executor.hpp` | Add one field to `ExecutionResult` so a cancelled turn is distinguishable from a failure |
| `include/pu/runtime.hpp` | An `AcpAgentClient` member and `GetOrCreateAcpClient()` |
| `src/runtime.cpp` | Dispatch in `ProcessInput`; create and destroy the client with the toolbox |
| `CMakeLists.txt` | New sources |
| The line parser | Moved to `core/` if phase 2 takes that route (see D4) |
| `docs/INTEGRATIONS.md` | New (phase 6) |

A separate `CodeBuddyConfig` is **not** wanted: `BackendConfig` already carries
`type`, `host`, `model` and `api_key`, and duplicating them would create two
sources of truth that drift.

---

## 5. Phases

Each phase is one commit and is reviewable on its own.

### Phase 1: protocol contract

A standalone harness, not production code, that answers D1 and D2 and records what
the protocol actually looks like. It may be a temporary `main` or a test binary
that is excluded from the default test run.

Tasks:

1. Call `initialize` against `POST /api/v1/acp` with `CODEBUDDY_API_KEY` and
   `CODEBUDDY_INTERNET_ENVIRONMENT=internal`.
2. Record the request and the response verbatim, including the `initialize` request
   shape the server accepts.
3. Call `session/new`; record the `sessionId` format.
4. Call `session/prompt` and capture the **raw SSE byte stream**.
5. Send `session/cancel` mid-turn and capture what follows.
6. Repeat 4 with a prompt that forces a tool call, to see the tool events.

Deliverable: `docs/design/acp-samples.md`, containing

- raw request and response transcripts, including SSE framing lines (`event:`,
  `data:`, `id:`) exactly as received;
- the resolved answer to D1 and D2, with the observation that settled them;
- the observed `protocolVersion` and `agentInfo`;
- every `sessionUpdate` variant seen, with its fields;
- whether `sessionId` is reused across prompts;
- the exact cancellation result, including whether late updates arrive after
  `session/cancel`;
- which inbound requests (`session/request_permission`, `fs/*`, `terminal/*`) the
  agent actually issues;
- the `_meta` keys present in `initialize`, if any.

Acceptance: the six questions in the original brief are each answered with an
observed transcript, not an inference.

### Phase 2: `AcpTransport`

```cpp
class AcpTransport {
 public:
  AcpTransport(std::string base_url, std::string api_key);
  // Sends one JSON-RPC message and feeds every SSE line of the response to
  // on_sse_line. The shape of the stream is whatever phase 1 found.
  void Send(const std::string& json_rpc_body,
            std::function<void(std::string_view)> on_sse_line,
            CancelToken cancel_token = nullptr);
 private:
  std::string base_url_;
  std::string api_key_;
  pu::http::BeastHttpClient http_;
};
```

Attaches `Content-Type: application/json`, `Authorization: Bearer <key>` and
`X-CodeBuddy-Request: 1`. Redirects nothing, retries nothing, logs no header.

Acceptance: unit tests with a mock HTTP client covering headers, body, and
cancellation; a test that a blank line ends an event, and that a `data:` payload
split across several fields is reassembled; one integration test that reaches
`initialize`.

### Phase 3: `AcpRpcClient`

Routing, not just request/response. Four categories must be handled:

| Incoming message | Handling |
| --- | --- |
| Has `id`, has `result` | Resolve the pending promise for that id |
| Has `id`, has `error` | Reject it with the JSON-RPC `code` and `message` |
| Has `id`, has `method` | An **inbound request**: hand to the request handler and send its result back |
| No `id`, has `method` | A **notification**: hand to the notification handler; never reply |

```cpp
class AcpRpcClient {
 public:
  using NotificationHandler =
      std::function<void(const std::string& method, const boost::json::value& params)>;
  // Returns the JSON-RPC `result`, or throws to produce an error reply.
  using RequestHandler =
      std::function<boost::json::value(const std::string& method,
                                       const boost::json::value& params)>;

  explicit AcpRpcClient(AcpTransport& transport);
  void SetNotificationHandler(NotificationHandler handler);
  void SetRequestHandler(RequestHandler handler);

  std::future<boost::json::value> SendRequest(const std::string& method,
                                              const boost::json::value& params);
  void SendNotification(const std::string& method,
                        const boost::json::value& params);
  void OnSseMessage(const std::string& line);
};
```

`SendNotification` exists because `session/cancel` is a notification and a promise
would never settle.

Acceptance: unit tests resolving and rejecting a promise, dispatching a
notification without replying, and answering an inbound request; a test that a
disconnect or a timeout rejects every pending promise rather than leaving callers
waiting.

### Phase 4: `AcpAgentClient`

```cpp
class AcpAgentClient {
 public:
  explicit AcpAgentClient(const config::BackendConfig& backend);
  ~AcpAgentClient();

  void Initialize();                      // initialize + session/new
  ExecutionResult Run(const std::string& prompt,
                      std::function<void(const std::string&)> on_chunk = nullptr,
                      ToolCallbacks tool_callbacks = {},
                      CancelToken cancel_token = nullptr);
  void Cancel();
};
```

- Reuses `content_callback` and `ToolCallbacks`; defines no callback type.
- Maps `session/update` to those callbacks: `agent_message_chunk` to `on_chunk`,
  `tool_call` to `on_start`, `tool_call_update` with a terminal status to `on_end`.
- Negotiates the version and refuses a mismatch (section 6).
- Parses extension events only when `initialize` advertised them.
- Answers `session/request_permission` according to D1.
- Maps `stopReason` to the result (section 7).

Acceptance: an integration test completing one turn against the real service; a
test triggering a tool call and observing the callbacks; unit tests for a version
mismatch, for an inbound permission request, and for a cancelled turn.

### Phase 5: Runtime integration

1. An `AcpAgentClient` member, created lazily and destroyed with the toolbox.
2. Dispatch in `ProcessInput`, **before** the existing
   `session->CreateProvider()` call. That call throws
   `"Unknown backend type"` for an unrecognised type, so a branch placed after it
   fails on the first message.
3. Creation and teardown alongside `RebuildToolbox`, and cleanup in
   `SwitchAgent` and `SwitchWorkspace`.
4. `PU_ENABLE_CODEBUDDY` enforcement, at one stated place (section 6.3).

Acceptance: `agents.json` with `type: "codebuddy"` holds a conversation from the
CLI; the Web UI streams, shows tool blocks and cancels; switching agents leaves no
client behind.

### Phase 6: configuration and documentation

1. The `agents.json` example (China region).
2. `docs/INTEGRATIONS.md` with setup, environment variables, and troubleshooting
   for 403, authentication failure and wrong endpoint.
3. `ARCHITECTURE.md`: the ACP path in the layer diagram and a component row.
4. `CONTRIBUTING.md`: any build note the new sources need.

---

## 6. Version and capability negotiation

### 6.1 Protocol version

`protocolVersion` is an integer MAJOR version and is negotiated, not merely
checked.

```cpp
// The request carries the version this client supports.
params["protocolVersion"] = kSupportedProtocolVersion;

// The response carries the version the agent will use.
const int agreed = json::ValueOrDefault<int>(result, "protocolVersion", 0);
if (agreed != kSupportedProtocolVersion) {
  // The specification says to close the connection and tell the user.
  throw RuntimeError("CodeBuddy ACP protocol version " + std::to_string(agreed) +
                     " is not supported; this client speaks " +
                     std::to_string(kSupportedProtocolVersion));
}
```

Note the field path: `result.protocolVersion`. `serverInfo` is an **MCP** field and
does not exist here; implementation details are `agentInfo` and `clientInfo`.

### 6.2 Capability detection

Capabilities are how a peer declares optional support, and anything omitted is
**unsupported**. Custom capabilities arrive in `_meta`.

```cpp
bool has_agent_teams = false;
if (json::HasKey(result, "_meta") && result.at("_meta").is_object()) {
  has_agent_teams = json::HasKey(result.at("_meta"), "agent_teams");
}
```

Extension events are parsed only when advertised. An unknown `sessionUpdate`
variant is ignored rather than treated as an error, so a server-side addition
cannot break a turn.

### 6.3 Feature switch

`PU_ENABLE_CODEBUDDY=1` enables the backend. The check belongs at one place, and
the placement has a visible consequence:

| Placement | Consequence |
| --- | --- |
| In `ParseBackendConfig` | One `codebuddy` entry makes the **whole** `agents.json` fail, so unrelated agents stop working |
| At the first use | The configuration loads, but the failure appears at the first message |

Neither is wanted on its own. The chosen behaviour: parsing **succeeds** and the
type is recognised, and the client construction refuses with a clear message
naming the switch. This keeps a misconfigured entry from taking down unrelated
agents, and reports the problem at the point the user selects it.

---

## 7. Errors, cancellation and lifecycle

### 7.1 Error mapping

| Source | Mapping |
| --- | --- |
| JSON-RPC `error` object | `has_error = true`, `error_message` from `message`, with `code` included |
| HTTP 4xx or 5xx | The thrown `HttpError` already carries the status and the response text; surface it unchanged |
| Transport failure mid-turn | `has_error = true` with the transport's message |

An error is reported to the caller and **not appended to the conversation**. This
repository has already been fixed once for storing a failed request as an
assistant turn, which made the conversation grow on every refusal.

### 7.2 Cancellation

- `Cancel()` sends `session/cancel` as a notification for the current session.
- The turn ends with `stopReason: "cancelled"`, which is **not** an error.
- Pending `session/request_permission` requests are answered with the `cancelled`
  outcome.
- Late tool call updates arriving after the cancel are accepted, not discarded.
- `ExecutionResult` gains `bool was_cancelled = false`, because the existing fields
  cannot express "stopped on request" without abusing `has_error`.

### 7.3 Lifecycle and concurrency

| Concern | Requirement |
| --- | --- |
| Construction | `initialize` and `session/new` happen once, on first use |
| Destruction | `Shutdown` during destruction; no message is sent after it |
| Agent switch | The client is destroyed and a new one created; no session is reused across agents |
| Workspace switch | The same, because the session belongs to the previous workspace |
| Thread safety | The RPC client's pending map is mutex-guarded; the transport is not shared between turns. State the contract in the header comment, as the context types do |
| Ordering | `serve` serialises requests with `io_mutex`; the ACP client may rely on that but must not require it |

### 7.4 Secrets

The API key appears only in the `Authorization` header. It must not be logged, and
neither request bodies nor SSE lines may be logged at a level above `trace`. The
existing providers log request bodies at `debug`, which is **not** acceptable for a
request carrying credentials.

### 7.5 Timeouts

`initialize` and `session/new` need a bounded wait, or a wrong endpoint hangs the
CLI. A turn's own duration is unbounded by design, since an agent may work for
minutes; the `CancelToken` is how it is stopped.

---

## 8. Constraints

1. Do not change the `LLMProvider` interface. ACP is a parallel path, not a
   provider subclass.
2. Introduce no new dependency. Boost.Beast, Boost.JSON and OpenSSL suffice.
3. Keep the existing suite passing: **167 tests**, under both a UTF-8 and a
   non-UTF-8 console. A test that needs the network must be excluded from the
   default run, or the suite stops being passable offline.
4. Comments explain why, in English, and describe current intent rather than
   history.
5. One commit per phase, each reviewable alone.
6. Do not modify `ChatMessage` or `Transcript`. They are frozen; this feature needs
   neither.
7. Branch: `feat/codebuddy-acp`.

---

## 9. Acceptance

- [ ] `docs/design/acp-samples.md` holds raw transcripts answering D1 and D2 and
      the six protocol questions
- [ ] `AcpTransport` unit tests pass; SSE framing checked against the samples,
      including a `data:` payload split across several fields
- [ ] `AcpRpcClient` unit tests pass, covering all four message categories and a
      disconnect that rejects pending callers
- [ ] `AcpAgentClient` completes one turn against the real service, and triggers a
      tool call with the callbacks observed
- [ ] An inbound `session/request_permission` is answered, not ignored
- [ ] Cancellation returns `was_cancelled` and does not set `has_error`
- [ ] CLI holds a conversation, and cancels one
- [ ] Web UI streams, renders tool blocks, and cancels
- [ ] A version mismatch produces the message from 6.1
- [ ] `PU_ENABLE_CODEBUDDY` behaves as 6.3 describes, in both states
- [ ] 167 tests still pass, and a new network-dependent test does not break an
      offline run
- [ ] `docs/INTEGRATIONS.md`, `ARCHITECTURE.md` and the `agents.json` example are
      updated

---

## 10. Decisions needed before phase 1

Two of these are prerequisites, not discoveries: the phase 1 harness has to know
what to probe for.

| # | Question | Why it blocks |
| --- | --- | --- |
| 1 | **D1: do CodeBuddy's tools run on the server, or through client `fs/*` and `terminal/*`?** | It fixes `clientCapabilities`, which determines what the client must implement and therefore the scope of phase 4 |
| 2 | **D2: request-scoped or session-scoped SSE?** | Phase 1 probes for the answer, but if it is session-scoped then `BeastHttpClient` needs extending before phase 2 can start |
| 3 | China-region base URL: `https://copilot.tencent.com` or another? | Only the probe can confirm; `codebuddy --help` may not expose it |
| 4 | Should `sessionId` be persisted in `session.json`? | Resuming needs `session/load`, which needs the `loadSession` capability. The v2 reader ignores unknown keys, so adding a field needs **no** schema bump - the cost is low either way |
| 5 | Model selection | `BackendConfig.model` already exists; leaving it empty uses the server default |
| 6 | API key format | No client-side validation is planned, so this is informational only |

Deliberately **not** in this list: where tools execute and what the transport shape
is. Both are decisions (section 3) that phase 1 resolves, because the interfaces
above harden around them.
