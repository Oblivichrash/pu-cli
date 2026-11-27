# Errors

What a command exits with, what it prints, and which exceptions exist. Every subcommand
answers within the same five statuses, and the hierarchy below them is small enough to
state in one page — it lived inside [ARCHITECTURE.md](ARCHITECTURE.md) for a while, where
it was three sections deep in a document about what the tree is made of.

## What a command exits with, and what it prints

Every subcommand answers with the same five statuses (`include/pu/app/cli.hpp`), separated
by what a caller should do next rather than by which layer failed:

| Code | Meaning | The caller's next move |
| --- | --- | --- |
| `0` | the command did what it was asked | nothing |
| `1` | the invocation or the configuration is wrong | fix it; a retry cannot help |
| `2` | the provider refused or could not be reached | retry may work |
| `3` | the sandbox refused every tool call the turn made | change the command or the policy |
| `4` | a person stopped it | neither retry nor fix |

A turn the sandbox refused and the model recovered from is `0`: the refusal is advice the
model read, not the turn's outcome. A refusal whose reason is that no channel could ask for
approval is `1`, not `3`, because the command is fine and the configuration is not.

stdout carries what was asked for — the reply, a model listing, a rendered config — and
stderr carries everything else: notices, usage, the reason a turn failed. `pu ask "... | ..."`
pipes the answer and nothing beside it.

## The frame every command runs inside

`cli::WithInitializedRuntime` loads the configuration, runs a body, and saves the session.
Every subcommand that needs a `Runtime` goes through it, so initialization and persistence
are stated once: a failure to initialize is `1`, because the configuration is what is wrong,
and a command that returns without persisting is the exception, not the rule. A non-empty
agent name overrides the configured agent for that run only.

Two signatures exist for a reason rather than for symmetry:

- `RunProbe` reads a product named on the command line without an `agents.json` entry — its
  host and key come from the gateway table, and neither the configuration nor the session is
  touched. That is what makes probing a gateway safe from inside a workspace already
  configured for another one.
- `RunSse` takes a URL, a method and a set of headers from the caller. A feed is not a chat
  turn, so nothing about it is read from `agents.json`; it borrows the runtime only for the
  `HttpClient` and the cancellation token.

`FormatSseEvent` is exposed rather than private because it is what a caller's pipe reads, and
a pipe is testable where a socket is not.

## Checked, not thrown

A condition the code can know in advance is checked, not thrown: an operation that can
fail visibly returns `std::optional` or `nullptr`. Exceptions are for what cannot be known
in advance — a body that does not parse, a peer that closes the socket, a provider that
answers with an error frame.

All non-recoverable runtime errors derive from one base class:

```
pu::RuntimeError : std::runtime_error
  ├── pu::RequestRefused        (the state cannot serve a well-formed request)
  └── pu::Error                 (the request or the configuration is wrong)
        └── pu::HttpError       (HttpClient failures)
```

`pu::RequestRefused` sits beside `Error` rather than under it: `pu serve` answers a refusal
with `400` and an `Error` with `500`, so deriving one from the other would report a
caller's mistake as a fault of this process. The status a route chooses, and the body that
carries it, are in [web-api.md](web-api.md).

## Where a catch is allowed

A catch earns its place only by changing the control flow — returning a value, retrying,
falling back, converting, or ending the process. Logging is not handling, and a catch
whose whole body logs before the path carries on as if nothing happened is the shape to
avoid. The three boundaries where reporting and continuing coexist:

| Boundary | Why a catch is there | What it carries |
|----------|---------------------|-----------------|
| A call site that parses | `json::parse` throwing is how a byte string says it is not JSON | the value the caller wants: a default, a fallback, a marked-invalid record |
| A background thread (`McpClient`, the WebSocket reader) | no caller exists above it to be told | the log is the report; the promise or socket carries the outcome |
| The process boundary (`main`, CLI entry points) | returning is no longer an option | the message is the entire response |

A failure that cannot be converted is raised, not answered with a plausible-looking empty
value: a `tools/list` that fails is not a server with no tools, so `McpClient::ListTools`
throws and `Runtime::RebuildToolbox` skips that server the way it skips one that never
connected.

## An absent file is not a failure

An absent `agents.json` is an ordinary state, not a failure: `config::FindConfigPath`
returns an empty string and `FindServeOptions` returns `std::nullopt`. Only
`Runtime::Initialize`, which cannot continue, turns the empty path into the `Error` naming
where the file goes. A function reports that it cannot answer rather than picking one
caller's condition to raise.

`Runtime::Initialize` resolves the configuration in one order: an explicit path argument,
then `PU_AGENTS_CONFIG`, then `./.pu/agents.json`, then the user configuration. The
environment variable is checked before the conventional locations so a test or a packaged
run can point at a fixture without a writable workspace, and it is an outright failure
when the file it names is missing — falling back to the conventional path would silently
run against the wrong configuration rather than the one the caller asked for.

The same holds for invariants: `Runtime::ActiveAgent` and `ConfiguredBackend` do not check
that `Initialize` ran, because every path reaching them passes through it. A test for a
condition that cannot be false asserts that the code distrusts itself.

## What a provider failure becomes

A transient provider failure is retried below the turn, not above it, and the statuses and
delays are in [providers.md](providers.md#retries). What matters here is that the retry
does not change the exit status: a call that succeeds on the second try is a `0`, and only
an exhausted retry reaches the `2` above. A mid-stream failure is the known exception —
content has already been handed to `on_content`, so a retry there would repeat it.
