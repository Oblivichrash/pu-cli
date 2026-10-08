# Provider differences

What each backend puts on the wire, and what it does with what comes back. This is
reference material: the architecture that decides which provider serves a request is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Which provider a request reaches

`config::CreateBackend` (`agent_config.cpp`) maps `BackendType` (`agent.hpp`)
to a concrete provider, and `Session::CreateProvider()` is its only caller. The
`agents.json` backend `type` field selects it: `"ollama"`, `"openai"` and
`"codebuddy"` are the values it accepts, anything else is refused at load time, and
an absent field means Ollama. "OpenAI compatible" means the `/chat/completions` SSE
contract and covers OpenAI, DeepSeek thinking mode, vLLM, the CodeBuddy cloud
gateway and compatible gateways.

A type is spelled in one place — `BackendTypeName()` / `ParseBackendType()` in
`agent.hpp` — because a stored session, an `agents.json` entry and an `/api/session`
answer all name it. Answering "is it OpenAI?" separately at each of those sites is
what turns a third type into a lie rather than a failure: the type was written out,
read back and reported as Ollama, while every layer involved believed it had agreed.

The CodeBuddy gateway is one of these, not a third protocol: the same stream at a
different base, plus a header set the gateway is called by. Those facts live in
`llm/codebuddy.hpp` and reach the request as
`OpenAIProvider::Config::extra_headers`, which is asked for once per request rather
than held, so a set carrying correlation ids is fresh for each one. Its errors arrive
in an envelope of its own, whose `msg` names the cause; `SummarizeErrorBody` reads
that shape beside the OpenAI one.

## Field by field

| Dimension | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Endpoint | `{host}/api/chat` | `{host}/chat/completions` |
| Auth | `Authorization: Bearer` only when a key is set | same |
| Streaming | NDJSON, one object per line, ends at `{"done":true}` | SSE, `data: ` lines, ends at `data: [DONE]` |
| Model / temperature | `model`, `options.temperature` | `model`, `temperature` |
| Token cap | not sent | `max_tokens` |
| Extra options | `keep_alive` (default `30m`, keeps the KV cache warm) | `extra_body.thinking.type = "disabled"` for the `none` level |
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

## Capabilities

| Capability | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Tools, streaming content, parallel calls | yes | yes |
| Streaming tool calls | whole call per line | index accumulation |
| Reasoning | `message.thinking` | `delta.reasoning_content` |
| Reasoning signature | no | no |
| Full provider response retained | no | no |
| Multimodal input or output | no | no |
| Prompt caching hints | `keep_alive` only | none |

Both report `SupportsTools() == true`, and tool schemas fall back to `{}` via
`ToolDefinition::Parameters()`.
