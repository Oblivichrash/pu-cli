# Gateway, protocol, and entry layout

This records the layer layout the refactor follows, and the routes it did not take.
The code that follows from it belongs to the files [CONTRIBUTING.md](../../CONTRIBUTING.md)
names for them; this document is the design rationale, so it is written once and then
left to describe what was decided rather than how it was implemented.

The first step of it is built: `src/gateways/` holds one file per gate behind the explicit
table, `Gateway` carries the id, the name, the default host and the two factories, and
`agents.json` names a `gateway`. The rest is still target rather than state — the struct
carries no `entries`, `headers` or `catalog`, no `entry` is read, and the schema version
in section 5 has not moved — so read each section as the shape the work is done towards,
and change this document rather than quietly diverging from it.

---

## 1. The problem

`BackendType` (`config/backend.hpp`) is one value answering three questions at once:

```
ollama    → the ollama protocol, the Ollama product, and the provider to build
openai    → the OpenAI protocol, a family of compatible gates, and the provider
codebuddy → the OpenAI protocol, the CodeBuddy product, and the same provider as openai
```

`codebuddy` is the proof that this does not hold: it speaks the OpenAI protocol
(`CreateBackend` builds an `OpenAIProvider` for it) but is listed as a third type
beside `openai`. Adding a gate means touching seven places — the enum, `DefaultHostFor`,
`BackendTypeName`, `ParseBackendType`, `BackendTypeNames`, the `tag_invoke` pair,
`CreateBackend`, and `ListModels`.

Two facts about vendors that look alike are not alike, and the refactor separates them:
a protocol is *how to talk*, a gateway is *which product*, and a connection is *where*.

## 2. The layout

```
Protocol   behaviour: endpoint path, request body, stream parse, listing parse
              openai, anthropic, ollama

Gateway    data: one file per product
              name, entries, default host, headers, catalogue shape

Entry      a binding: one gateway × one protocol × one default host
```

A gateway states the protocols it offers, because one product may offer more than one:
DeepSeek publishes both an OpenAI-compatible and an Anthropic-compatible endpoint.
Configuring one names the gateway and, when a gateway offers more than one entry, which
entry.

`provider` stays what it is in the code today — the implementation of a protocol. It is
not the gateway: one `OpenAIProvider` serves OpenAI, DeepSeek, NVIDIA and CodeBuddy.

## 3. What a gateway file holds

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

## 4. Adding and removing a gate

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
  than defaulting, which today's `ParseBackendType(...).value_or(kOllama)` does not do.
- `entry` is optional and defaults to the gateway's first.
- `model` is required: no default is honest about a choice only the account can make.
- `host` is optional and overrides the entry's default. It stays overridable because a
  gateway has more than one (a staging base beside a production one) and a self-hosted
  endpoint has no default at all.
- `api_key` is optional and unchanged.

`kSessionSchemaVersion` moves from 7 to 8. A stored `type` maps to the gateway of the
same name, so `ollama`, `openai` and `codebuddy` keep reading.

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
- **Splitting the protocol out of the provider.** Attempted, then reverted, and deferred
  until a compile failure with no explanation is understood. The work was to move
  `openai_provider.cpp` (297 lines) into `openai_protocol.cpp` piece by piece, leaving the
  provider with the session-level work; restart it that way rather than rewriting the file
  from memory. Three probes, all against the same `MakeOpenAI(...)` whose return type is
  `std::unique_ptr<pu::LLMProvider>`:

  ```
  decltype(p) is unique_ptr<pu::LLMProvider>        // holds
  LLMProvider& p = *MakeOpenAI(...); p.Chat(req);   // compiles
  auto p = MakeOpenAI(...);          p.Chat(req);   // error: no member named 'Chat'
  ```

  The preprocessed output says the same translation unit holds `LLMProvider` complete,
  with `virtual ChatResult Chat(const ChatRequest&) = 0;`. The three facts contradict each
  other and the cause is not known, so nothing here explains it; it is recorded so that the
  next attempt starts from the measurement rather than from the assumption.
