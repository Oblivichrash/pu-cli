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

`GET /v2/models`, `GET /config/models` and `GET /v2/config` do **not** exist (404 from
the gateway), so a listing cannot be read from the chat base. The model catalogue lives
at the base's `/v3/config`, below, and that is what `ListModels` reads for this type.

## 2. Model listing

```
GET {origin}/v3/config
```

The `/v2` segment a turn's URL carries belongs to the chat route, not to the origin: the
catalogue hangs off `/v3` beside it. Appending the path to the chat base asks for
`/v2/v3/config` and is answered `404 Route Not Found`; the same path on the origin is
answered 200. So the catalogue URL is the chat base with its trailing version segment
replaced, which is what `CodeBuddyCatalogUrl` (`include/pu/llm/model_listing.hpp`)
does rather than concatenating.

The same headers as a turn (section 3), which is why the gateway builds them and hands them
to the lister rather than the lister naming them. `Authorization` carries the account key
and is
the only header shown to matter: a bare `Authorization` request was answered 200, as were
runs dropping `Accept`, `User-Agent` and each of the client's other headers in turn. (An
early reading held `User-Agent` to be required; it came from a request that also carried
`Accept`, and the bare run above does not reproduce it.) The client's header set is still
what is sent, for the reason section 4 gives. A successful answer is

```json
{"code":0,"msg":"OK","requestId":"...",
 "data":{"agents":[{"name":"cli","models":["hy4-preview","deepseek-v4.1-flash", "..."]}],
         "enterpriseId":"","models":[{"id":"deepseek-v4-pro","name":"Deepseek-V4-Pro","maxInputTokens":1000000,"...":...}]}}
```

Two lists answer two different questions and are not the same set:

| Field | Answers |
| --- | --- |
| `data.models[].id` | Every model the account is offered, images and all; each entry also carries `credits`, `maxInputTokens`, `maxOutputTokens`, `supportsImages`, `supportsToolCall` |
| `data.agents[].models[]` | The subset the named agent may run, which is what a chat turn can name |

`ListModels` returns the agent's list, because that is the one a turn can use; the wider
catalogue is a service fact a caller has no verb for. A model in the wider list and not
the agent's is one this client cannot reach by naming it.

`GET /v2/accounts` answers 200 and is the cheapest way to check a credential:

```json
{"code":0,"msg":"OK","requestId":"...","data":{"accounts":[{"uid":"...","nickname":"...","uin":"...","type":"personal","lastLogin":true}]}}
```

## 3. Authentication

`Authorization: Bearer <key>`, where the key is the same `ck_...` string the CLI and
the IDE use. No exchange or refresh is needed for a request: a key that authenticates
returns 200, and the server reports the identity it resolved in its `X-User-Id`
response header.

## 4. Headers

The set is the one the installed client sends and every captured request used:
the sixteen in `src/config/gateway.cpp`, with the bearer token, `Accept`
and `Content-Type` headers the HTTP layer adds. Only
`Authorization` is required: dropping any other header, or all of them, still
returns 200, and the gateway answers with nothing but the key present. The rest
are the client's own telemetry and product routing, sent to match the client
exactly rather than to satisfy the server.

The `x-stainless-*` trio and the `CLI/1.0.7` self-identification are what the client
sends because it is generated from an SDK; the conversation and request ids are
correlation values the server records rather than echoes. The three per-call ids are
one shape, a uuid with its dashes removed, spliced from the client's generator rather
than a second id format, while the two values that name a thing rather than an event
keep their dashes. The gateway in `src/config/gateway.cpp` is the source for that shape
and for each id's lifetime.

## 5. Request body

An OpenAI chat-completions body. Two things differ from a plain OpenAI call:

- **`stream` is effectively required.** The third-party bridge forces `stream: true`
  before forwarding, with the comment that the endpoint only answers streamed. The
  provider sends it unconditionally for every backend, so nothing has to change here.
- **Two messages minimum.** The same bridge prepends a system message when the request
  carries a single user message. This is not implemented: the provider sends the
  history it is given, so a session whose first turn has no system prompt sends one
  message. No request has been observed to fail that way, and the bridge's rule is
  recorded here as the reason to keep a system prompt in play rather than as a step
  this code takes.

`tools` is accepted and honoured: a request carrying one `function` tool produced a
call for it (section 7).

`temperature` and `max_tokens` are accepted. `reasoning_effort` is honoured — it is what
makes this gateway reason at all — while `extra_body.thinking` is accepted and inert.
[codebuddy-fieldnotes.md](codebuddy-fieldnotes.md) records how each was measured.

## 6. Response stream

`text/event-stream`, one JSON object per `data:` line, terminated by `data: [DONE]`.
Blank lines separate events. Every chunk is an OpenAI-shaped

```json
{"id":"...","model":"deepseek-v4-flash","object":"chat.completion.chunk","created":1791459858,
 "choices":[{"index":0,"delta":{...},"logprobs":null,"finish_reason":""}]}
```

and every chunk carries a full `usage` object — not only the last one — including
`prompt_tokens`, `completion_tokens`, `total_tokens`, a `completion_tokens_details`
breakdown and a `credit` figure. `credit` is the call's cost, not an account balance:
it was `0` for a one-token answer and `0.05` for a 203-token one.

The chunk `id` is per-response, not per-conversation: three turns sent under one
`X-Conversation-ID` came back with three different ids. So no part of the response
carries the conversation's identity back, and `X-Conversation-ID` is a correlation
value the server records rather than one it echoes. What it buys is not observable
from a single client.

`delta` fields seen:

| Field | Meaning |
| --- | --- |
| `role` | `assistant`, on the first chunk and again on a later one |
| `content` | the answer, as fragments |
| `reasoning_content` | the thinking, as fragments — the DeepSeek field the repository already reads |
| `tool_calls` | an array, empty on chunks that carry no call |
| `function_call` | present and empty on one chunk, `{"name":"","arguments":""}` |

## 7. Tool calls

The first fragment of a call carries the identity; the rest carry only the argument
text and the index. Reassembling every fragment in order gives the call's arguments,
so the assembly is the one [providers.md](../providers.md) already describes for the
OpenAI path — `index`-keyed, tolerating fragments without an `id` or a `type`. The turn
ends with `finish_reason: "tool_calls"`.

```json
{"id":"call_00_5tZoah0yC3OkEkSONqY82854","type":"function","function":{"name":"get_weather","arguments":""},"index":0}
{"function":{"name":"","arguments":"{"},"index":0}
{"function":{"name":"","arguments":"\"city\""},"index":0}
```

Nothing in the observed stream is CodeBuddy-specific except the header set and the
error envelope: text, reasoning and calls all arrive in the shapes an OpenAI-compatible
backend produces.

## 8. Errors

A rejected request answers non-200 with an envelope of its own, and the provider has to
read two fields out of it: `displayMsg` is written for a user and is what gets shown,
`msg` names the specific cause and is what belongs in a log.

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

What the provider does with a non-200 body: `displayMsg.en` becomes the error a user
reads, and `msg`, `code` and `requestId` go to the log at warning. A body carrying no
`displayMsg` falls back to `msg`, which is what every other gateway already relies on.
