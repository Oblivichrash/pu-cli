# Gateway, protocol, and entry layout

This records the design behind the gateway layout, the routes it did not take, and the one
layer it deferred. What is built is described in
[ARCHITECTURE.md](../ARCHITECTURE.md#gates-protocols-and-providers); what is not is here,
so this document carries only the reasoning rather than the state.

**Built:** `src/gateways/gateways.cpp` holds the gateway table — one row per gateway, not one
file per gateway — `Gateway` carries the id, the name, the default host and the two factories,
and `agents.json` names a `gateway` — required, with an unknown or missing name refused.

**Deferred:** the `entry` layer — a gateway offering more than one protocol, bound by a config
field of that name. Every gateway measured so far speaks exactly one protocol, so the layer
would be unused state. Sections 3 and 5 describe the shape it would take; they are not
current behaviour, and nothing in them should be read as built.

---

## 1. The problem

`BackendType` (`config/backend.hpp`) is one value answering three questions at once:

```
ollama    → the ollama protocol, the Ollama product, and the provider to build
openai    → the OpenAI protocol, a family of compatible gateways, and the provider
codebuddy → the OpenAI protocol, the CodeBuddy product, and the same provider as openai
```

`codebuddy` is the proof that this does not hold: it speaks the OpenAI protocol
(`CreateBackend` builds an `OpenAIProvider` for it) but is listed as a third type
beside `openai`. Adding a gateway means touching seven places — the enum, `DefaultHostFor`,
`BackendTypeName`, `ParseBackendType`, `BackendTypeNames`, the `tag_invoke` pair,
`CreateBackend`, and `ListModels`.

Two facts about vendors that look alike are not alike, and the refactor separates them:
a protocol is *how to talk*, a gateway is *which product*, and a connection is *where*.
That problem is solved for the OpenAI protocol: the seven places are the table plus a file
per gateway, and §2 records the layout that replaced them. The rest of this section is kept as
the problem the layout was written against.

## 2. The layout

```
Protocol   behaviour: endpoint path, request body, stream parse, listing parse
              openai, anthropic, ollama

Gateway    data: one file per product
              name, entries, default host, headers, catalogue shape

Entry      a binding: one gateway × one protocol × one default host
```

What is built of this today: the `openai` protocol is `include/pu/llm/openai_protocol.hpp`
and `src/llm/openai_protocol.cpp`, and `OpenAIProvider` (`openai_provider.{hpp,cpp}`) is the
shell a gateway builds — the protocol at that gateway's host and with that gateway's headers. The
protocol layer is a step of the pipeline rather than a thing beside it, because it reads
the stream through `StreamingProvider`'s virtuals and writes into the pipeline's
accumulators; what it does not hold is a host or a set of headers, which is the gateway's.

`ollama` is not split this way: its protocol and its provider are still one class
(`ollama_provider.{hpp,cpp}`). The listing parse is not part of either protocol class — it
is `CatalogShape` data read by `src/llm/model_listing.cpp`, which is where it belongs,
since a catalogue is read once and not over a stream.

`provider` is what the code makes it — the implementation of a protocol, not the gateway: one
`OpenAIProvider` serves OpenAI, DeepSeek, NVIDIA and CodeBuddy.

Had a gateway needed to offer more than one protocol — one product may publish an
OpenAI-compatible and an Anthropic-compatible endpoint — configuring it would name the
gateway and, since it offers more than one, which entry. That is the deferred layer, and
the rest of this section is its shape.

## 3. What a gateway file would hold, once a gateway offers more than one protocol

```
Gateway {
  name           "codebuddy"
  entries[]      { protocol, default_host }
  headers[]      sent on both a turn and a catalogue read
  catalog        { path, container, id, nested?, capability field names }
}
```

A gateway that needs the catalogue on a different origin than its chat route states its
own catalogue URL rather than a path appended to the chat base. This is not a
hypothetical: `/v3/config` is a sibling of the chat route's `/v2`, and appending the path
answered 404 (see [codebuddy-cloud-api.md](codebuddy-cloud-api.md#2-model-listing)).

## 4. Adding and removing a gateway

Adding one is a new file plus one line in the table. Removing one is the same in reverse.
No `Register` call, no static initialisation, no global registry: a file nobody names is
a file the linker drops, and `ARCHITECTURE.md` states the code holds no global state.
The table is explicit, so the linker always sees it.

## 5. Configuration

```json
"backend": {
  "gateway": "deepseek",
  "entry": "anthropic",
  "model": "deepseek-chat",
  "host": "https://api.deepseek.com",
  "api_key": "${DEEPSEEK_API_KEY}"
}
```

- `gateway` is required and names a file in the table. An unknown name is refused rather
  than defaulting, and so is a missing one; a backend that named no gateway would otherwise
  be answered by whichever gateway happened to be the default.
- `entry` is optional and defaults to the gateway's first.
- `model` is required: no default is honest about a choice only the account can make.
- `host` is optional and overrides the entry's default. It stays overridable because a
  gateway has more than one (a staging base beside a production one) and a self-hosted
  endpoint has no default at all.
- `api_key` is optional and unchanged.

The `entry` field is not read today, and the section above describes the shape it would
take rather than a change made. `kSessionSchemaVersion` is still 7: a stored `type` maps to
the gateway of the same name, so `ollama`, `openai` and `codebuddy` keep reading, and the
version moves to 8 only if `entry` is built.

## 6. Capability

A model's capability is a property of the host, not of the configuration: it is read from
the catalogue, cached, and treated as unknown when the read fails. A field a gateway does
not report stays unknown rather than false, so a caller does not read "not stated" as
"measured not to support it".

Where each gateway keeps those fields, and what it calls them, is `CatalogShape` data, so
the protocol parser names no vendor. The measured requirement behind one of them: a model
whose `onlyReasoning` is true is answered `400 11150` when a request carries no
`reasoning_effort`, and answers 200 to any level — so a level is required rather than one
particular level ([codebuddy-fieldnotes.md](codebuddy-fieldnotes.md)).

The one caller that acts on the field today is `pu models --verify`: such a model is not
asked at all when the backend sends no level, since the request cannot succeed, and the
reason it was not asked is reported in place of the host's own refusal. A turn is not
refused this way — nothing on that path reads the catalogue — so this is as far as the
field reaches until a capability is available where a turn is built.


## 7. Who decides which backend a turn uses

Four sources, resolved once, highest first:

```
the command line   --agent, or a named gateway
the session        what the user last chose
agents.json        default_agent
the gateway        its default host
```

A caller that names an agent is not answered by a different one; this was a defect and is
fixed in the same branch (`fix: let a command line agent outrank the session's`). The
resolution belongs in one function so that a new entry point — a Web selector, a
`--gateway` flag — cannot reintroduce it.

## 8. Routes not taken

- **An `entry` layer between a gateway and a protocol.** Deferred, not dropped: no gateway
  measured so far speaks more than one protocol, so the field would be state a user can set
  wrong with nothing to gain. It comes back when a product is found that publishes two
  protocols worth choosing between — DeepSeek's OpenAI- and Anthropic-compatible endpoints
  are the candidate, and neither is served here yet.
- **A `Register` call per gateway file.** Dropped: it needs a global registry and an
  `OBJECT` library to survive linking, against `ARCHITECTURE.md`'s "no global state".
- **`protocol` as a configuration field.** Dropped: every gateway measured so far
  implies exactly one protocol, so the field would be redundant state a user can get
  wrong. It comes back only if a gateway is found that offers a protocol chosen by the
  request rather than by the entry.
- **A gateway class with a virtual `ListModels` per vendor.** Dropped as a first step:
  the listing logic is protocol-level, and only the path and field names differ, so a
  per-vendor class repeats what a shape describes.
- **Appending the catalogue path to the chat base.** Dropped: measured 404.
- **Reading the first protocol-split attempt's failure as a compiler defect.** It was not
  one. The probes recorded against that attempt do not contradict each other: one called
  `.Chat()` on a `unique_ptr` where `->` was needed, and the compiler named the type it was
  calling on ("is not a member of `std::unique_ptr<pu::LLMProvider, ...>`"). The same line
  with `->` compiles, and the reference form compiles because it dereferences first.
  `LLMProvider`'s completeness — the thing the preprocessed output was read to check — does
  not enter into looking a member up on a `unique_ptr`. The split was then done (§2); this
  stays so that the phantom blocker is not re-derived from the first attempt's notes.
