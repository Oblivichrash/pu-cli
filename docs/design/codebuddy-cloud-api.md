# CodeBuddy cloud API

What a native `codebuddy` backend has to speak. Every fact here was observed, not
inferred: the shapes come from the request the installed CodeBuddy client makes, and
the responses were captured by calling the endpoint directly with a real account key.
The probes that did so were scratch files, kept only for as long as they were being read.

This replaces the ACP route: ACP is served by a local `codebuddy --acp` child process, so
a client would need that CLI installed, while this endpoint is reachable on its own and
is already the surface the installed client uses. The code side of this — the type's
name, where the base and the header set live — is in [providers.md](../providers.md).

What was measured *about* this gateway rather than required *of* it — which fields are
honored, which models answer, and what is still unresolved — is in
[codebuddy-fieldnotes.md](codebuddy-fieldnotes.md). Those findings describe one account
at one time and are not part of the contract below.

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

`temperature` and `max_tokens` are accepted. `reasoning_effort` is honoured — it is what
makes this gateway reason at all — while `extra_body.thinking` is accepted and inert.
[codebuddy-fieldnotes.md](codebuddy-fieldnotes.md) records how each was measured.

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

A rejected request answers non-200 with an envelope of its own, and the provider has to
read two fields out of it: `displayMsg` is written for a user and is what should be
shown, `msg` names the specific cause and is what belongs in a log.

```json
{"code":11102,
 "msg":"model [deepseek-v3] service info not found",
 "requestId":"...",
 "displayMsg":{"en":"Model unavailable. Please switch models","zh":"当前模型不可用，请切换模型","zh-hant":"..."},
 "displayTips":{"en":"...","zh":"...","zh-hant":"..."},
 "actions":["SWITCH_MODEL","SUBMIT_FEEDBACK","RETRY"]}
```

A single `code` covers more than one cause — 11102 is raised both for a model that does
not exist and for one this account may not use, told apart only by `msg`. The codes seen
so far are recorded in [codebuddy-fieldnotes.md](codebuddy-fieldnotes.md).
