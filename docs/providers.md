# Provider differences

What each backend puts on the wire, and what it does with what comes back. This
is reference material: which layer decides the provider is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Which provider a request reaches

A gateway answers for a backend type. `src/gateways/gateways.cpp` holds the gateway table, and
a row states that gateway's name, default host, provider and catalogue read; `CreateBackend`
and `ListModels` (`config/backend.hpp`) only look the type up in that table, so neither spells
a gateway's endpoint or provider itself. Adding a gateway is a row, and removing one is the
reverse. `Gateways()` is the list a build carries, so it is the answer to "which gateways does
this build know" rather than any sentence here.

The OpenAI protocol itself is a layer below the gateway: `src/llm/openai_protocol.cpp` holds
the endpoint, the request body and the SSE parse, and `OpenAIProvider`
(`openai_provider.{hpp,cpp}`) is the shell that puts one host and one set of headers on it.
So the `openai` and `codebuddy` gateways build the same protocol, and an agent of the `openai`
gateway changes only the host, the headers and the response shape.

The gateways a build knows are the rows `Gateways()` returns, and the refusal lists them, so a
misspelt name answers with the set rather than a pointer to this page. An entry that names
none of them is refused at load time, as is one that names no gateway at all. "OpenAI
compatible" covers OpenAI, DeepSeek thinking mode, vLLM and compatible gateways, all of which
are the one `openai` gateway with a different `host`.

A gateway is spelled in one place for the same reason as before — a stored session, an
`agents.json` entry and an `/api/session` answer all name it, and answering "is it
OpenAI?" separately at each site is what turns a third gateway into a lie rather than a
failure. Whether a gateway carries a thinking level follows from the gateway, and no provider
subclass overrides it.

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

`core::SseParser` (`include/pu/core/sse.hpp`) is that format's implementation, and it is a
pure function of the bytes fed to it: a caller tests it without a socket, and the same
parser serves a chat gateway and `pu sse` because the framing belongs to the format rather
than to whoever is sending. The details a caller has to know, none of which the format
states:

| Question | Answer |
| --- | --- |
| A `\n` landed mid-character because the read split the sequence | The line waits for the rest of it. Handing the half on would corrupt it, so the parser holds what is incomplete until any split of the stream is whole again |
| The stream ended on a frame with no blank line | It is still delivered. A server that closes after its last `data:` line sent that line, so `Finish()` reports it rather than dropping it |
| A `retry:` field arrived | It is reported, not acted on. The caller owns the reconnect policy, and a parser that slept would be one a caller could not test |
| A gateway sends newline-delimited JSON | `SetBareLinesAreEvents` makes a line with no field name an event whose payload is the line. A stream carrying a `data:` line is an event stream regardless, so the setting only decides what to do with a bare line |
| Two `data:` lines in one frame | Joined with a newline. A payload split over several lines is one payload, and the split is transport rather than content |
| A frame with no `data:` line at all | Not an event. `event` and `id` name an event, they do not make one |

### Usage

Field names are not hardcoded in the reader: `ResponseShape`
(`include/pu/llm/openai_protocol.hpp`) names every comparison a reply is read with, and a
provider config carries the shape its gateway speaks. The default is the OpenAI-compatible
spelling, so a gateway that renames nothing states nothing. `ProviderCapabilities` is the
counterpart for a request.

| Field | Where it is read from |
| --- | --- |
| the two counts the turn is shown against | `prompt_tokens` / `completion_tokens`, or Ollama's `prompt_eval_count` / `eval_count` |
| `total_tokens` | the provider's own total. A backend that reports none leaves it `0` rather than having it summed from the two counts above |
| the cached prompt | `prompt_tokens_details.cached_tokens` where nested, else the top-level `prompt_cache_hit_tokens`; Ollama names it `prompt_eval_cached_count` |
| the call's cost | read only from the field the product's gateway names in `Gateway::billing`, and only where it prices one |

A name left empty in a shape means the field does not exist on that gateway.

**What a call cost is a product's answer, not a protocol's.** Two gateways share the OpenAI
wire and only one of them reports a price, so the field it arrives in is a property of the
product: `config::Gateway::billing` names the field and the unit, and a gateway that prices
nothing leaves it `kNone`. CodeBuddy reports `credit`, an amount of its own credits for this
call — not an account balance, and not comparable to another product's number. No conversion
is done anywhere, because no rate exists in this tree that could make two products' units
comparable. A call a product priced at zero and a product that prices nothing are different
answers: the first has a cost of `0`, the second has none, and only that distinction makes a
summed total worth reading.

Ollama reports no usage object: the counts and durations ride on the final line, which is
kept whole rather than filtered to a list of names.

The whole object as the gateway sent it is kept on `ChatResult::raw_usage`, so a field no
row here names is carried rather than dropped. Nothing reads it beyond the `debug` log
that prints it: it is a record, not a feature.

`ChatResult::unread_fields` names the keys on the first frame that no code reads — a signal,
not a failure, since a gateway may add a field at will. The tests pinning a captured stream
assert the set whole, so a new field on a gateway we hold a capture of fails the build. The
comparison is one level deep: a key nested inside `choices` or `message` is not seen.
`seen_fields` is the complement it is diffed against, and `missing_fields` is the third
case: a name `ResponseShape` says this build reads that the reply never carried, which is
what says a gateway stopped sending a field rather than that a field was added.

A count a backend does not report stays `0` rather than being derived. `total_tokens` in
particular is the provider's own total: summing the two counts beside it would produce a
figure the provider never gave, which reads as an answer where there is none. `cached_tokens`
is the prompt the provider answered from its cache, and nothing is inferred from it either.

### Reasoning blocks

A reasoning-capable gateway returns its thinking beside the answer, and `ReasoningBlock`
carries both forms: `text` is what the model thought, `raw` is the provider's own encoding
kept replayable, and `signature` is the signed proof an API attaches to a block. A signature
must be returned verbatim on the next request — a re-serialised block fails verification —
which is why `raw` exists rather than a reconstruction from the parsed fields.

### Completions

The request asks for one completion, so the reply is read from the first choice `index`
the stream names. A gateway that sends a second index has it reported at `debug` rather
than folded into the reply.

## Deadlines

Every request is bounded, and the bound is why a gateway that hangs is a failure rather than a
wait. `BeastHttpClient` runs each operation asynchronously and drives it with `io_context::run()`,
because beast's `expires_after` covers asynchronous operations only and asio's synchronous calls
block where no timer can reach them. Three budgets, named at the top of
`src/core/beast_http_client.cpp`:

| Budget | Value | Bounds |
| --- | --- | --- |
| `kConnectBudget` | 10s | Reaching the host: DNS, TCP, and the TLS handshake |
| `kStreamIdleBudget` | 300s | One read of a turn — an idle deadline, not a cap on the reply, so a long answer may run for as long as it keeps producing bytes |
| `kReadIdleBudget` | 30s | One catalogue or health read, which is control plane rather than a model answering |

A missed deadline is reported as its own sentence — `HTTP read timed out after 30s` — rather than
as the cancelled socket operation underneath it, since the first names something a reader can act
on. NVIDIA NIM is the reason this exists: several of its ids accept a connection and send no byte
at all, which used to hold `pu models --verify` and `pu ask` for as long as the process lived.

## Limits

Neither backend implements a capability the table above does not name. Both
stream content and accept parallel tool calls; neither keeps a full provider
response, carries a reasoning signature, or accepts multimodal input or output.

Reasoning is stored as an ordered list of blocks, not one string, so a turn that
thinks, calls a tool, then thinks again keeps that order. A block carries the
`text`, an optional `signature` the provider wants replayed, and the provider's own
`raw` JSON when it round-trips. Whether a block list reaches the wire as one string
or as a list is decided per provider, as the signatures row says.

### What each backend says about a context window

Each was asked rather than assumed, and the answers are observations that drift rather than a
contract to code against. `pu check --caps` prints them, and that is their whole use: the page's
context bar is drawn from `backend.context_window` in `agents.json` rather than from here. A
number worth writing there is the one this table's endpoint reports — with CodeBuddy's caveat
below in mind, since it states 1M and enforces less.

| Backend | Endpoint | Answer |
| --- | --- | --- |
| DeepSeek | `GET /v1/models` | `context_window: 1048576` on each model, read by the OpenAI shape |
| Ollama Cloud | `POST /api/show` | a `model_info` key ending in `context_length`; no catalogue endpoint states one, so this gateway states none to `pu` |
| Ollama Cloud | `GET /api/ps` | `401 unauthorized`, so the loaded-model window is unreachable |
| NVIDIA NIM | `GET /v1/models` | 80 models, keys are only `created`, `id`, `object`, `owned_by` — no window at all |
| NVIDIA NIM | `GET /v1/metadata` | `404 page not found` |
| CodeBuddy | `GET /v3/config` | `data.models[].maxInputTokens`, read into the catalogue's capabilities |

Ollama's key prefix varies with the model's architecture, so a reader has to match
the suffix rather than a fixed name. Measured across five models — `gptoss.`,
`nemotron_h_moe.` and `gemma4.` for the first three, and none at all for
`nemotron-3-ultra`, which a lookup naming `gptoss.` therefore misses.

Because the figure is advisory, a nominal one that is not what the product enforces costs a
misleading bar rather than a failed turn. CodeBuddy is the case in point: it truncates instead
of refusing, so a request carrying two million characters was answered `200` with a normal reply
whose counts (`prompt_tokens: 250004` against `prompt_cache_hit_tokens: 249856` plus
`prompt_cache_miss_tokens: 148`) put the cache ceiling at 250,000 tokens. The gateway never said
so, and nothing on the client side can detect it from the status alone — which is why the
enforced figure is the one to write in `context_window` for a CodeBuddy entry.
