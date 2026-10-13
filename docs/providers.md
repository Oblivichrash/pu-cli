# Provider differences

What each backend puts on the wire, and what it does with what comes back. This
is reference material: which layer decides the provider is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Which provider a request reaches

`CreateBackend` (`config/backend.hpp`) maps the `agents.json` backend `type` to a concrete
provider and is the only path `Session` uses to obtain one. Accepted values are
`ollama`, `openai`, and `codebuddy`; anything else is refused at load time, and
an absent field means Ollama. "OpenAI compatible" covers OpenAI, DeepSeek
thinking mode, vLLM, and compatible gateways. The same file owns `ListModels`, which reads each type's listing endpoint - `/api/tags` for Ollama, `/models` for the OpenAI-compatible ones.

A type is spelled in one place — `BackendTypeName()` / `ParseBackendType()` in
`config/backend.hpp` — because a stored session, an `agents.json` entry and an
`/api/session` answer all name it; answering "is it OpenAI?" separately at each
site is what turns a third type into a lie rather than a failure. The same rule
covers capability: whether a backend carries a thinking level follows from its
type, and no provider subclass overrides it.

`codebuddy` is one of these, not a third protocol: the same stream at a different
base URL plus a header set. Its gateway behavior is recorded in
[design/codebuddy-cloud-api.md](design/codebuddy-cloud-api.md).

## Field by field

| Dimension | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Endpoint | `{host}/api/chat` | `{host}/chat/completions` |
| Auth | `Authorization: Bearer` only when a key is set | same |
| Streaming | NDJSON, one object per line, ends at `{"done":true}` | SSE, `data: ` lines, ends at `data: [DONE]` |
| Model / temperature | `model`, `options.temperature` | `model`, `temperature` |
| Token cap | not sent; Ollama's own default stands, and `max_tokens` has no effect here | `max_tokens` when set |
| Extra options | `keep_alive` is a fixed `30m` (not configurable) | nothing beyond the level below |
| Thinking level | top-level `think`: `false` for `none`, the level name for `low`/`medium`/`high`, not sent for `default` | `extra_body.thinking.type = "disabled"` for `none`, `reasoning_effort` for `low`/`medium`/`high`, nothing for `default`. A gateway that does not reason by default has nothing for the first field to suppress, so it is inert there — [codebuddy's gateway](design/codebuddy-cloud-api.md) is one |
| Role mapping | `user`/`assistant`/`system`/`tool`; anything else falls back to `user` | `tool_result` rewritten to `tool`; others verbatim |
| Assistant with tool calls | `content` sent as-is | `content` forced to `null` |
| Reasoning on request | sent on assistant messages as `thinking` when non-empty | sent on assistant messages as `reasoning_content` when non-empty |
| Tool result fields | `role`, `tool_name`, `tool_call_id` | `role`, `tool_call_id` |
| `tool_calls.arguments` | JSON object; a string is parsed, non-JSON passed through | JSON string; an object or array is re-serialised |
| Call assembly | one complete call per line | `index`-keyed deltas, flushed when the stream ends, sentinel or not; a call with no `index` is a call of its own when it carries an `id` |
| Reasoning on response | `message.thinking` accumulated | `delta.reasoning_content` accumulated |
| Reasoning on the wire | one string under `thinking` | one string under `reasoning_content` |
| Reasoning with signatures | n/a | a provider that signs its blocks gets `[{text, signature?}]` instead of one string, replayed verbatim when a block carries the provider's own JSON |
| End of reply | `done_reason` on the final object | `finish_reason` on each choice |
| Error inside the stream | `{"error":"..."}` raised as the request's failure | `{"error":{...}}` raised as the request's failure |
| Error outside the stream | a non-200 body is summarized into the failure detail | same; a gateway that sends `displayMsg` has it shown in place of the log line, and its `msg`, `code` and `requestId` are logged at warning |
| Usage | `prompt_eval_count` / `eval_count` on the final object | `usage`, which the request has to ask for |

The level names are newer than the setting they name: until `low`/`medium`/`high`
existed it was a boolean, and `default` is what that switch's on-state meant while
`none` is the off-state it replaced.

## What comes back

The table above covers what goes out. This is what is read from the answer, and where a
field it does not name ends up.

### SSE framing

An OpenAI-compatible stream is SSE: a line is `field: value` with one optional space after
the colon, a line starting with `:` is a comment, `event`/`id`/`retry` name the event
rather than carry it, and the event ends at a blank line. Only `data` is read, and a
payload split over several `data:` lines is joined until what they hold parses — a
gateway that writes `data:{...}` with no space reads the same as one that writes
`data: {...}`. Ollama's NDJSON frames nothing, so a blank line there says nothing.

### Usage

Field names are not hardcoded in the reader: `ResponseShape`
(`include/pu/llm/response_shape.hpp`) names every comparison a reply is read with, and a
provider config carries the shape its gateway speaks. The default is the OpenAI-compatible
spelling, so a gateway that renames nothing states nothing. `ProviderCapabilities` is the
counterpart for a request.

| Field | Where it is read from |
| --- | --- |
| the two counts the turn is shown against | `prompt_tokens` / `completion_tokens`, or Ollama's `prompt_eval_count` / `eval_count` |
| `total_tokens` | the provider's own total. A backend that reports none leaves it `0` rather than having it summed from the two counts above |
| the cached prompt | `prompt_tokens_details.cached_tokens` where nested, else the top-level `prompt_cache_hit_tokens`; Ollama names it `prompt_eval_cached_count` |
| `credit` | the call's cost, where the gateway prices one |

A name left empty in a shape means the field does not exist on that gateway.

Ollama reports no usage object: the counts and durations ride on the final line, which is
kept whole rather than filtered to a list of names.

The whole object as the gateway sent it is kept on `ChatResult::raw_usage`, so a field no
row here names is carried rather than dropped. Nothing reads it beyond the `debug` log
that prints it: it is a record, not a feature.

`ChatResult::unread_fields` names the keys on the first frame that no code reads — a signal,
not a failure, since a gateway may add a field at will. The tests pinning a captured stream
assert the set whole, so a new field on a gateway we hold a capture of fails the build. The
comparison is one level deep: a key nested inside `choices` or `message` is not seen.

### Completions

The request asks for one completion, so the reply is read from the first choice `index`
the stream names. A gateway that sends a second index has it reported at `debug` rather
than folded into the reply.

## Limits

Neither backend implements a capability the table above does not name. Both
stream content and accept parallel tool calls; neither keeps a full provider
response, carries a reasoning signature, or accepts multimodal input or output.

Reasoning is stored as an ordered list of blocks, not one string, so a turn that
thinks, calls a tool, then thinks again keeps that order. A block carries the
`text`, an optional `signature` the provider wants replayed, and the provider's own
`raw` JSON when it round-trips. Whether a block list reaches the wire as one string
or as a list is decided per provider, as the signatures row says.

### No backend here reports a context window

Each was asked rather than assumed, and the answers are observations that drift
rather than a contract to code against. Nothing reads them; they are kept because
they explain why a window is not shown anywhere.

| Backend | Endpoint | Answer |
| --- | --- | --- |
| DeepSeek | `GET /v1/models` | `context_window: 1048576` on each model |
| Ollama Cloud | `POST /api/show` | a `model_info` key ending in `context_length` |
| Ollama Cloud | `GET /api/ps` | `401 unauthorized`, so the loaded-model window is unreachable |
| NVIDIA NIM | `GET /v1/models` | 80 models, keys are only `created`, `id`, `object`, `owned_by` |
| NVIDIA NIM | `GET /v1/metadata` | `404 page not found` |
| CodeBuddy | `GET /v2/models`, `/v3/config` | `models` is `null`; the endpoint answers 200 and names no models |

Ollama's key prefix varies with the model's architecture, so a reader has to match
the suffix rather than a fixed name. Measured across five models — `gptoss.`,
`nemotron_h_moe.` and `gemma4.` for the first three, and none at all for
`nemotron-3-ultra`, which a lookup naming `gptoss.` therefore misses.

CodeBuddy truncates instead of refusing: a request carrying two million characters
was answered `200` with a normal reply whose counts (`prompt_tokens: 250004` against
`prompt_cache_hit_tokens: 249856` plus `prompt_cache_miss_tokens: 148`) put the
cache ceiling at 250,000 tokens. The gateway never said so, so a request that
outgrows the window is answered as though it fit and nothing on the client side can
detect it from the status alone.
