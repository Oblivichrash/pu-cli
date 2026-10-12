# Provider differences

What each backend puts on the wire, and what it does with what comes back. This
is reference material: which layer decides the provider is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Which provider a request reaches

`CreateBackend` (`config/backend.hpp`) maps the `agents.json` backend `type` to a concrete
provider and is the only path `Session` uses to obtain one. Accepted values are
`ollama`, `openai`, and `codebuddy`; anything else is refused at load time, and
an absent field means Ollama. "OpenAI compatible" covers OpenAI, DeepSeek
thinking mode, vLLM, and compatible gateways.

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
| Thinking level | top-level `think`: `false` for `none`, the level name for `low`/`medium`/`high`, not sent for `default` | `extra_body.thinking.type = "disabled"` for `none`, `reasoning_effort` for `low`/`medium`/`high`, nothing for `default` |
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
| Usage | `prompt_eval_count` / `eval_count` on the final object | `usage`, which the request has to ask for |

The level names are newer than the setting they name: until `low`/`medium`/`high`
existed it was a boolean, and `default` is what that switch's on-state meant while
`none` is the off-state it replaced.

## Limits

Neither backend implements a capability the table above does not name. Both
stream content and accept parallel tool calls; neither keeps a full provider
response, carries a reasoning signature, or accepts multimodal input or output.

Reasoning is stored as an ordered list of blocks, not one string, so a turn that
thinks, calls a tool, then thinks again keeps that order. A block carries the
`text`, an optional `signature` the provider wants replayed, and the provider's own
`raw` JSON when it round-trips. Whether a block list reaches the wire as one string
or as a list is decided per provider, as the signatures row says.
