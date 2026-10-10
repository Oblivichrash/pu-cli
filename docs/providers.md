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
site is what turns a third type into a lie rather than a failure.

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
| Token cap | not sent | `max_tokens` |
| Extra options | `keep_alive` is a fixed `30m` (not configurable) | `extra_body.thinking.type = "disabled"` for the `none` level |
| Thinking level | not sent; the model decides for itself | `reasoning_effort` for `low`/`medium`/`high`, nothing for `default` |
| Role mapping | `user`/`assistant`/`system`/`tool`; anything else falls back to `user` | `tool_result` rewritten to `tool`; others verbatim |
| Assistant with tool calls | `content` sent as-is | `content` forced to `null` |
| Reasoning on request | never sent | sent on assistant messages when non-empty |
| Tool result fields | `role`, `tool_name`, `tool_call_id` | `role`, `tool_call_id` |
| `tool_calls.arguments` | JSON object; a string is parsed, non-JSON passed through | JSON string; an object or array is re-serialised |
| Call assembly | one complete call per line | `index`-keyed deltas, flushed when the stream ends, sentinel or not; a call with no `index` is a call of its own when it carries an `id` |
| Reasoning on response | `message.thinking` accumulated | `delta.reasoning_content` accumulated |
| End of reply | `done_reason` on the final object | `finish_reason` on each choice |
| Error inside the stream | `{"error":"..."}` raised as the request's failure | `{"error":{...}}` raised as the request's failure |
| Usage | `prompt_eval_count` / `eval_count` on the final object | `usage`, which the request has to ask for |

The level names are newer than the setting they name: until `low`/`medium`/`high`
existed it was a boolean, and `default` is what that switch's on-state meant while
`none` is the off-state it replaced.

## Capabilities

Neither backend keeps a full provider response, carries a reasoning signature, or
accepts multimodal input or output.

| Capability | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Tools, streaming content, parallel calls | yes | yes |
| Thinking level | no | yes |
| Streaming tool calls | whole call per line | index accumulation |
| Reasoning | `message.thinking` | `delta.reasoning_content` |
| Prompt caching hints | `keep_alive` only | none |
