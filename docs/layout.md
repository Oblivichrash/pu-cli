# Layout

Where a file goes, which library it links, and which direction an `#include` may point.
These are the three facts a contributor needs before writing the first line of a new
module, and they were a section near the end of [ARCHITECTURE.md](ARCHITECTURE.md) — a
place a reader arrives three hundred lines after the decision it would have informed.

The layering is checked by the linker rather than by review, so most of this document is
describing a constraint that a build failure already enforces. It is stated anyway,
because a failure that arrives as an undefined symbol is a poor way to learn which
library a new file belongs in.

## The tree

The tree is layered bottom-up: `core/` holds dependency-free base utilities, the domain
modules sit beside it, and the orchestration headers/modules live at the root of
`include/pu/` and `src/`.

```
include/pu/                  src/
├── agent_tools/             ├── agent_tools/          # Toolbox, the tools themselves
├── app/                     ├── app/                  # main, CLI parsing
├── config/                  ├── config/               # agents.json, the backend model, the gateways
├── core/                    ├── core/                 # logging, platform, HTTP client, running a command
├── llm/                     ├── llm/                  # protocols, providers, streaming parser
├── mcp/                     ├── mcp/                  # transports, JSON-RPC client
├── paging/                  ├── paging/               # the buffers a session can still read
├── runtime/                 ├── runtime/              # runtime, executor, command router
│   ├── command_router.hpp   ├── serve/                # `pu serve`: listener, REST routes, WebSocket
│   ├── executor.hpp         ├── session/              # Session, Conversation, message graph
│   └── runtime.hpp          └── tools/                # the commands `pu` ships: probe, models, sse
├── session/
└── tools/
```

Two directories are called `tools`, and the distinction is who calls them. `agent_tools/`
holds the tools the model invokes — `bash`, `write_file`, `ask_dir` — as classes built
against `Tool` and registered in a `Toolbox`. `tools/` holds the separate executables a
human or a shell invokes: `pu-probe`, `pu-models`, `pu-sse`, plus the entry point and exit
codes they share. The model reaches those through `bash`, on `PATH`, as any other command.

A header lives in its module's directory; `include/pu/runtime/` holds the orchestration
layer (`Runtime`, `Executor`, `CommandRouter`). A header whose module has no directory of
its own stays beside the sources that include it.

`web/` is the page the server mounts, served straight from the directory the server
resolved, so adding a module means adding the file and its `<script>` tag and nothing
else. A target that names a directory, or is not there, is a `404` rather than a read of
the directory. Its module split is described in [web-ui.md](web-ui.md).

## The libraries

The build is static libraries, one per layer, each linking only the ones below it. It
branches rather than forming a single chain, and the branches are the point: a library
that does not need a session does not link one.

```
                              pu_core
                                 │
                             pu_serve
                                 │
                             pu_runtime
                       ┌─────────┴─────────┐
               pu_agent_tools            pu_mcp
                       │                   │
                   pu_session              │
                    ┌──┴──┐                │
               pu_config │  └── pu_paging ─┘
                    │              │
                pu_model           │
                    │              │
                 pu_llm            │
                    └──────┬───────┘
                     pu_core_util
```

`pu_probe` and `pu_models` hang off `pu_config` without touching `pu_session`, which is
why they are drawn as edges in the table below rather than in the picture.

`pu_paging` links `pu_core_util` directly and `pu_session` links it beside `pu_config`,
which is why the bottom of the picture is a join rather than a single trunk.

Read as edges rather than as a picture — from `CMakeLists.txt`:

| Library | Links |
|---|---|
| `pu_core_util` | nothing (the bottom) |
| `pu_llm` | `pu_core_util` |
| `pu_model` | `pu_llm` |
| `pu_paging` | `pu_core_util` |
| `pu_config` | `pu_model` |
| `pu_probe` | `pu_config` |
| `pu_models` | `pu_config` |
| `pu_session` | `pu_config`, `pu_paging` |
| `pu_mcp` | `pu_config` |
| `pu_agent_tools` | `pu_session` |
| `pu_runtime` | `pu_agent_tools`, `pu_mcp` |
| `pu_serve` | `pu_runtime` |
| `pu_core` | `pu_serve`, `pu_probe`, `pu_models` |

`pu_core_util` (logging, platform, process, SSE, the HTTP client) is the bottom and
`pu_core` is the top, holding only the command layer. The `pu` executable links
`pu_core`, and so do `pu-sse`, `pu-probe` and `pu-models` at their own levels — the
three side-tools are separate executables rather than subcommands spawned back into the
same binary. The split is what makes the layering a fact the linker checks rather than a
habit the includes happen to keep: a module that reaches upwards fails to link, and the
failure names the symbol.

`pu_config` answers what `agents.json` says — the agent table and the list of servers a
turn may reach — and with the provider-reading layers carved out it no longer reaches a
provider at all. It also holds the one rule for how a session-free tool picks a gateway,
because that rule is a reading of the table rather than of a turn, and the two binaries
that read a provider without a session would otherwise each carry their own answer to the
same question.

`pu_probe` and `pu_models` are layers of their own because each reads a provider: one
capture and two reports from the probe, one catalogue and the entry it would write from
the models layer, and no session, transcript or pager in sight in either. They sit beside
`pu_config` rather than above it, because neither needs to know what a turn is — and they
are siblings, so a helper both of them want belongs in `pu_config`, not in one of the
two.

## Dependency direction

Includes point one way: `core/` is a leaf, `config/` and `llm/` build on it, and `mcp/`,
`agent_tools/`, `session/`, and the orchestration headers build on those. No directory
includes one that sits above it, and `config/` in particular never reaches `mcp/`.

The rule holds because a record belongs to the layer that *parses* it, not the layer that
*uses* it. `config/mcp_server.hpp` and `config/security_policy.hpp` are leaf headers
holding plain data — `McpServerConfig` (read from `agents.json`) and `SecurityPolicy` — so
`mcp/client.hpp` and `agent_tools/tool.hpp` include them rather than the reverse. Before
the split both were declared next to their consumers, which made `agent_tools/` depend on
`config/` depending on `mcp/`: a cycle that pulled the whole transport layer into any
header naming a `Tool`.

### The one link that goes the other way

`pu_agent_tools` calls `RunDelegatedTurn`, declared in `agent_tools/delegation.hpp` and defined in
`runtime.cpp`, so `pu_agent_tools` links a weak fallback that answers "no turn can run here" and
`pu_runtime` links the definition that replaces it. A tool states a question; the layer
that can answer one supplies the answer. Without that seam `ask_dir` would have to
construct a `Runtime`, which is the one upward dependency the tools would otherwise carry.
No header under `tools/` appears in that picture: the commands sit above `pu_core`'s own
libraries and are reached only by the four executables.

## Gates, protocols and providers

Three facts about a vendor that look alike are three different things: a **protocol** is
*how to talk*, a **gateway** is *which product*, and a connection is *where*.

`model/gateway.hpp` holds the middle one: `GatewayId` names the gateways a build carries,
`Gateway` is `{id, name, default_host, api_key_env, protocol, endpoint_path,
make_provider, make_listing, listing_headers, billing}`, and `Gateways()` / `FindGateway()`
/ `GatewayForId()` answer from one explicit table. Every gateway lives in
`src/llm/gateway_table.cpp` with its neighbours: a gateway is mostly a table row plus the
mapping from the backend config to its protocol's config, and those mappings are only
readable as a set — side by side, a field one gateway fills and another drops is visible,
and in one file per gateway it is not. `MakeOpenAIProvider` and `MakeOllamaProvider` share
`FillProtocolConfig`, which is why the Ollama mapping cannot drift from the OpenAI one.

The table is written where the factories it names are visible — above `model`, in `llm` —
and the reader below lives in `model`. A program therefore installs the table once, from
its own entry point, with `model::InstallGatewayTable()`: `main.cpp` and the test runner
both do, and a lookup made before that falls back to the one `llm` wrote, so the install
is an optimisation of the direction rather than a trap.

Billing follows the same split. What a call costs is read in the protocol layer, through
`ResponseShape`: one gateway names the field `credit`, another names nothing at all, and
the difference never leaves `TokenUsage::credit`. The upper layers never branch on which
gateway is in use — there is no `if (cfg.gateway == kCodeBuddy)` in the code above the
protocol, and a turn's usage is the sum over every round its tool loop ran, accumulated in
the executor.

`agents.json` names a gateway through `gateway`, and a missing or unknown name is refused
rather than defaulted, so a backend never reaches a host its author did not choose. The
field is what a stored config carries; `BackendTypeName()` / `ParseBackendType()` are
gone, and one parser resolves the name.

A protocol stays what it was — the implementation of how a gateway talks. `openai` is
split into `llm/openai_protocol.{hpp,cpp}` (the wire format) and `OpenAIProvider` (that
protocol at one host with one set of headers), because a change to the SSE parse and a
change to a gateway's headers are different changes; `ollama` keeps protocol and provider
in one class. `anthropic` is a third protocol rather than a variant of `openai`: the text
arrives as a list of content blocks rather than one string, the frame type is named by the
SSE `event:` field rather than a field inside the JSON, the system prompt is a request
field rather than a message, and the key travels in `x-api-key` rather than a bearer.
`AnthropicProvider` is therefore the one place in the layer where two representations of a
message meet, and it is the file to read first when that translation goes wrong. The
gateway abstraction stops short of one thing the design chose not to build yet: a gateway
offering more than one protocol, which would need an `entry` between the gateway and the
protocol. Every gateway measured so far speaks exactly one, so that layer is deferred
rather than carried as unused state. [providers.md](providers.md) is the per-field table.

## Adding one

| What | Where it lands |
|---|---|
| A new gateway | A row in `src/llm/gateway_table.cpp`; a product that speaks an existing protocol needs no new class. [ARCHITECTURE.md](ARCHITECTURE.md#extension-points) has the full recipe. |
| A new module directory | A new CMake target at the layer it belongs to, linking only downward, plus its `include/pu/<module>/` beside its `src/<module>/`. |
| A new file in an existing module | That module's directory. Nothing else changes. |
