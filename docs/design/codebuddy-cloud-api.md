# CodeBuddy cloud API

What a native `codebuddy` backend has to speak. Every fact here was observed, not
inferred: the shapes come from the request the installed CodeBuddy client makes, and
the responses were captured by calling the endpoint directly with a real account key.
The probes that did so were scratch files, kept only for as long as they were being read.

This replaces the ACP route: ACP is served by a local `codebuddy --acp` child process, so
a client would need that CLI installed, while this endpoint is reachable on its own and
is already the surface the installed client uses. The code side of this — the type's
name, where the base and the header set live — is in [providers.md](../providers.md).

---

## 1. Endpoint

```
POST {base}/chat/completions        base = https://copilot.tencent.com/v2
```

`staging-copilot.tencent.com/v2` is the staging base, and the client carries both.
`GET /v2/models`, `GET /config/models` and `GET /v2/config` do **not** exist (404 from
the gateway), so the base cannot be used to list models.

`GET /v2/accounts` answers 200 and is the cheapest way to check a credential:

```json
{"code":0,"msg":"OK","requestId":"...","data":{"accounts":[{"uid":"...","nickname":"...","uin":"...","type":"personal","lastLogin":true}]}}
```

## 2. Authentication

`Authorization: Bearer <key>`, where the key is the same `ck_...` string the CLI and
the IDE use. No exchange or refresh is needed for a request: a key that authenticates
returns 200, and the server reports the identity it resolved in its `X-User-Id`
response header.

## 3. Headers

The set below is the one the installed client sends, and the one every captured
request used. Which of them are strictly required has **not** been isolated.

```http
Authorization: Bearer <key>
Accept: application/json
Content-Type: application/json
X-Requested-With: XMLHttpRequest
X-Domain: www.codebuddy.ai
X-Product: SaaS
X-Agent-Intent: craft
X-IDE-Type: CLI
X-IDE-Name: CLI
X-IDE-Version: 1.0.7
User-Agent: CLI/1.0.7 CodeBuddy/1.0.7
X-Conversation-ID: <uuid>
X-Conversation-Request-ID: <32 hex>
X-Conversation-Message-ID: <uuid, no dashes>
X-Request-ID: <32 hex>
X-User-Id: <uuid>
x-stainless-lang: js
x-stainless-package-version: 5.10.1
x-stainless-runtime: node
```

The `x-stainless-*` trio and the `CLI/1.0.7` self-identification are what the client
sends because it is generated from an SDK; the conversation and request ids are
correlation values the server echoes.

## 4. Request body

An OpenAI chat-completions body. Two things differ from a plain OpenAI call:

- **`stream` is effectively required.** The third-party bridge forces `stream: true`
  before forwarding, with the comment that the endpoint only answers streamed.
- **Two messages minimum.** The same bridge prepends a system message when the request
  carries a single user message. Not reproduced here yet, but a request that always
  carries a system prompt is on the safe side of it.

`tools` is accepted and honoured: a request carrying one `function` tool produced a
call for it (section 6).

What the fields the OpenAI provider already sends do here. Each was tried as the
baseline request plus one difference:

| Field | Result |
| --- | --- |
| `stream: true` | required in practice |
| `temperature`, `max_tokens` | accepted |
| `reasoning_effort: "high"` | **accepted and honoured**. The same prompt answered `ok` with no reasoning at all on the baseline; with this field the stream carried `reasoning_content` fragments (`"We"`, `" need"`, …) |
| `extra_body: {"thinking":{"type":"disabled"}}` | accepted (200), and inert on this gateway: the default request already emits no reasoning for a prompt that benefits from one, so there is nothing for the field to suppress |
| `stream_options: {"include_usage": true}` | accepted (200), and the `usage` object arrives per chunk with or without it |

## 5. Response stream

`text/event-stream`, one JSON object per `data:` line, terminated by `data: [DONE]`.
Blank lines separate events. Every chunk is an OpenAI-shaped

```json
{"id":"...","model":"deepseek-v4-flash","object":"chat.completion.chunk","created":1791459858,
 "choices":[{"index":0,"delta":{...},"logprobs":null,"finish_reason":""}]}
```

and every chunk carries a full `usage` object — not only the last one — including
`prompt_tokens`, `completion_tokens`, `total_tokens`, a `completion_tokens_details`
breakdown and a `credit` figure.

The chunk `id` is the conversation's rather than the response's: five requests sent
under one `X-Conversation-ID` came back with the same `id` and only `created`
differing, so that header is what groups a conversation on the server.

`delta` fields seen:

| Field | Meaning |
| --- | --- |
| `role` | `assistant`, on the first chunk and again on a later one |
| `content` | the answer, as fragments |
| `reasoning_content` | the thinking, as fragments — the DeepSeek field the repository already reads |
| `tool_calls` | an array, empty on chunks that carry no call |
| `function_call` | present and empty on one chunk, `{"name":"","arguments":""}` |

## 6. Tool calls

The first fragment of a call carries the identity; the rest carry only the argument
text and the index:

```json
{"id":"call_00_5tZoah0yC3OkEkSONqY82854","type":"function","function":{"name":"get_weather","arguments":""},"index":0}
{"function":{"name":"","arguments":"{"},"index":0}
{"function":{"name":"","arguments":"\"city\""},"index":0}
```

Reassembling every fragment of that call in order gives `{"city": "Beijing"}`. So a
call is assembled by `index`, tolerating fragments without an `id` and without a
`type`, which is the same assembly the OpenAI provider path already does after the
work in `8144ae0`. The turn ends with `finish_reason: "tool_calls"`.

Nothing in the observed stream is CodeBuddy-specific except the header set and the
error envelope: text, reasoning and calls all arrive in the shapes an OpenAI-compatible
backend produces.

## 7. Errors

A rejected request answers non-200 with an envelope of its own:

```json
{"code":11102,
 "msg":"model [deepseek-v3] service info not found",
 "requestId":"...",
 "displayMsg":{"en":"Model unavailable. Please switch models","zh":"当前模型不可用，请切换模型","zh-hant":"..."},
 "displayTips":{"en":"...","zh":"...","zh-hant":"..."},
 "actions":["SWITCH_MODEL","SUBMIT_FEEDBACK","RETRY"]}
```

`displayMsg` is written for a user and is what should be shown; `msg` names the
specific cause and is what belongs in a log. `code` 11102 covers both an unknown model
and one the account is not entitled to, distinguished only by `msg`:

```
model [claude-opus-4.6] is only available for authorized users
```

## 8. Models

Observed on this account: **`deepseek-v4-flash`** answers 200 (and so does
`deepseek-r1`). Unknown ids — including every dated Claude id the client carries as a
string — answer 11102.

The ids the installed client names, for reference: `deepseek-v3`, `deepseek-v3-0324`,
`deepseek-v4-flash`, `deepseek-v4-flash-202605`, `deepseek-v4-pro`,
`deepseek-v4-pro-202606`, `deepseek-r1`, `deepseek-reasoner`, `claude-3-5-sonnet-20241022`,
`claude-3-7-sonnet-20250219`, `claude-4.5`, `claude-opus-4.6`, `claude-sonnet-4.5`,
`gemini-2.5-flash`, `gemini-3.1-flash`. Being named there does not mean the account may
use it: entitlement is the server's answer, and only a request settles it.

## 9. Still open

- Which headers are actually required, and which are the client's own telemetry.
- Whether `extra_body.thinking` would suppress reasoning if this gateway ever reasoned by
  default: it does not, so the field is inert here rather than wrong.
- Whether a model list can be discovered for an account at all.
- What `usage.credit` means for a session, and whether it is worth surfacing.
- Whether `/v2/auth/token/refresh` is needed for keys that expire; a request with a
  valid key needs no refresh.
