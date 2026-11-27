# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **single-session auto‑persistence** and **dynamic backend switching**.

Internals and layering live in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); the
[document index](#docs) lists the rest.

---

## Quick Start

### Build Dependencies

Runtime: **Boost >= 1.84** (Beast, Asio, JSON, ProgramOptions), **spdlog**, and
**OpenSSL**. Catch2 is needed only for tests. The exact package lists live in
[.github/workflows/ci.yml](.github/workflows/ci.yml); the common installs are:

```bash
sudo apt-get install -y libboost-dev libboost-system-dev \
  libboost-program-options-dev libboost-json-dev libspdlog-dev libssl-dev catch2  # Debian/Ubuntu
brew install boost spdlog openssl catch2                                   # macOS
vcpkg install boost-asio boost-beast boost-json boost-program-options spdlog openssl catch2  # Windows
```

### Build

A C++23 compiler and CMake >= 3.21 are required. Point CMake at your vcpkg
toolchain on Windows (`-DCMAKE_TOOLCHAIN_FILE=...`).

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --parallel --output-on-failure
```

Each test case runs in its own process, so the suite parallelizes without
coordination; the fixtures name their temporary directories after the process id,
since one that reused a fixed name would delete another test's workspace mid-run.

### Develop and install

The build tree's `pu` runs from the checkout and finds the page in `web/` beside it,
so day-to-day development needs no install step:

```bash
PU_WEB_DIR=web ./build/pu serve
```

An install puts the binary and the page under one prefix, and the installed binary
reads the page from the prefix it was installed into:

```bash
cmake --install build --prefix /path/to/pu-install   # bin/pu + share/pu/web/
```

Editing `web/` after an install means installing again — the installed copy is not
a live link into the checkout, and a binary running against its own prefix serves
the copy it was installed with.

### From nothing to a first answer

The build produces a binary that does nothing until it is told which model to talk to.
Three steps, in this order, and the second is the one that always fails when it can be
made to fail at all — so it comes before the one that costs money.

**1. Point `pu` at a model.** A local Ollama needs no key:

```bash
mkdir -p .pu
cat > .pu/agents.json <<'JSON'
{
  "name": "local-assistant",
  "backend": { "gateway": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:2b" }
}
JSON
```

A hosted gateway takes the same shape with `"gateway": "openai"` (or another product this
build carries) and a key in the environment rather than the file — see
[Environment Variables](#environment-variables). `gateway` must name a product, not a
protocol; naming a protocol is refused at load rather than guessed at.

A local `llama.cpp` server is the second keyless one, under its own name and port:

```bash
llama-server -m model.gguf --port 8080

cat > .pu/agents.json <<'JSON'
{
  "name": "local-assistant",
  "backend": { "gateway": "llamacpp", "model": "model.gguf" }
}
JSON
```

The `/v1` the server listens under is the gateway's default host
(`http://localhost:8080/v1`), so the entry names no address at all. It speaks the OpenAI
wire, so nothing else differs — but the name is its own, so `pu probe`, `pu models` and the
session's own record say `llamacpp` rather than borrowing `openai`'s.

**2. Prove it resolves before spending a turn.** This reads the config, expands the
environment references, and reaches the gateway's model list without running a
conversation:

```bash
./build/pu check
```

A failure here is a config error, and it is much cheaper to read than a failure in the
middle of a turn.

**3. Ask one question.**

```bash
./build/pu ask --prompt "list the files here, one per line" --show-usage
```

`--show-usage` prints what the turn cost on stderr, so a first run tells you both that it
worked and what it cost. Drop it when the answer is all you wanted.

For a conversation instead of a single question, `./build/pu chat`. Both append to the same
auto-saved session under `.pu/session.json`; there is no stateless mode yet, so an `ask`
from a script joins the workspace's conversation rather than standing apart from it. See
[Core Commands](#core-commands) for how to rewind or switch agent.

## Core Commands

| Command | What it does |
| --- | --- |
| `pu ask [--agent NAME] "..."` | One turn with no memory: the prompt is the whole of what it knows |
| `pu chat [--agent NAME]` | A conversation that is kept, with `/commands` |
| `pu delegate --agent NAME "..."` | One question put to another agent, answered in a turn of its own |
| `pu agents [--json]` | The configured agents, and which one answers |
| `pu models [--agent NAME] [--gateway GW] [--model M] [--detail ids\|caps] [--entry ID] [--verify]` | What the agent's gateway reports, and which model is active |
| `pu probe` *(developer tool, not in `pu --help`)* | A gateway's raw stream, kept, and the field names it actually sent |
| `pu check [--config PATH]` | What an `agents.json` resolves to, without contacting a provider |
| `pu setup` | Ask for the gateway, URL, key and model, and write `./.pu/agents.json` |
| `pu serve` | The Web UI and its WebSocket |
| `pu sse URL [--format text\|json\|raw]` | Read a `text/event-stream` from any endpoint and print its events |

Every command takes `-C DIR`, spelled and meant the same way as `git -C`: run as if started
in that directory. A `.pu/` directory is a whole world — its own agents table, its own
session, its own sandbox — so `-C` is how one directory asks another without either learning
about the other.

`pu delegate` is the same path a model takes when it hands a turn to another agent with
`ask_dir`: the question is the whole of what that turn knows, and the answer comes back without
the reading it took to reach one. A model may only hand a turn to an agent its own table lists
in `delegates`; by hand you may name any of them, and `--show-usage` prints what the turn cost.
Naming a directory instead works too, but only one that already carries its own `.pu/agents.json`
— nothing creates it for you, so the first move is to write that file and the second is to ask.
The delegated turn is one hop: it gets no `ask_dir` of its own.

The three commands that read a provider answer different questions. `pu models` asks what a
gateway offers and names the model to pick; `pu probe` asks what the answer is shaped like and
keeps the bytes; `pu sse` asks nothing of a gateway at all — it is a plain client that sends the
URL and headers it is given, reads no `agents.json`, and therefore reaches a feed that is not a
chat turn.

### The tools that ship beside `pu`

Installing `pu` installs these too, in the same directory. Each is also a subcommand, and the
two entry points run the same implementation — so an answer from `pu-sse` is an answer from
`pu sse`.

| Tool | Same as | What it needs |
| --- | --- | --- |
| `pu-sse URL [--format text\|json\|raw]` | `pu sse` | Nothing but the URL |
| `pu-probe [--gateway G] [--model M]` | `pu probe` | `agents.json`, to find a gateway |
| `pu-models [--gateway G] [--detail ids\|caps] [--entry ID] [--verify]` | `pu models` | `agents.json`, to find a gateway |

`pu-sse` is the case the split exists for: reading a feed needs an endpoint, not a workspace,
a session or a configuration file. A script that only wants to watch an event stream runs
`pu-sse` without `pu` having been set up at all.

`pu-probe` is the narrower one: it reads the agent table to find a host and a key, then sends
one prompt and prints what came back. No session, no transcript, no pager — a question about
a wire format does not need a conversation to answer it, and a ResponseShape for a new
gateway is written from this capture rather than from a specification.

`pu-models` answers the third question of the three: which models this gateway offers, without
opening a conversation to find out. Its `--entry` shape prints an `agents.json` entry ready to
paste, built from the same table `pu models --entry` reads, so a model added by hand and a model
added from a script describe themselves the same way. `--verify` asks each listed model for one
token and exits non-zero if any stays silent, which is what makes it usable as a pre-flight
check in a script rather than only as something to read.

The three side-tools share a rule rather than a code path: each reads `agents.json` only to
learn which gateway it is talking to, and none of them opens the workspace's session. That is
why they are separate binaries at all — `pu models`, `pu probe` and `pu sse` behind a session
would make "what can I pick" cost the same as "say this", and it does not.

Every command exits with one of five statuses and writes only its answer to stdout; both are
in [docs/errors.md](docs/errors.md).
`pu --help` names every command with the one line that says what it answers, and lists only the
flags that every command takes — `--bind` and `--verify` belong to one command each, so they are
on that command's page rather than in the map a reader arrives at with no command chosen.
`pu <command> --help` prints that command's own page.

## Docs

| Document | What it holds |
|----------|---------------|
| [docs/configuration.md](docs/configuration.md) | `agents.json`, environment variables, logging |
| [docs/web-api.md](docs/web-api.md) | `pu serve`: the WebSocket protocol and the REST endpoints |
| [docs/web-ui.md](docs/web-ui.md) | The browser modules, and how the page is served |
| [docs/paging.md](docs/paging.md) | `execute_bash` page by page: the envelope, cursors, and what a turn keeps |
| [docs/security.md](docs/security.md) | The trust boundary, loopback-only routes, sandbox and approval patterns |
| [docs/providers.md](docs/providers.md) | What each gateway puts on the wire |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, the runtime, the tool loop, MCP, persistence, extension points |
| [docs/layout.md](docs/layout.md) | The directory tree, the CMake targets, and which layer may link which |
| [docs/runtime.md](docs/runtime.md) | One `Runtime`: its lifecycle, cancellation contract, the tool loop, streaming, and the handler rules |
| [docs/errors.md](docs/errors.md) | Exit statuses, the exception hierarchy, and where a catch is allowed |
| [docs/limitations.md](docs/limitations.md) | What the program does not do, and which of those is by design |
| [docs/upgrading.md](docs/upgrading.md) | Which keys moved, version by version, and what an older session file does |
| [docs/releases.md](docs/releases.md) | How a release is cut, and what a CHANGELOG entry is |
| [docs/design/next.md](docs/design/next.md) | What is left to build, in order, and what was decided against |
| [docs/design/serve-transport.md](docs/design/serve-transport.md) | Why `pu serve` is one port per workspace, one WebSocket per page, and no build step |
| [docs/design/codebuddy-cloud-api.md](docs/design/codebuddy-cloud-api.md) | One gateway's wire format, and the two refusals it can answer |
| [docs/design/cli-agent-workflow.md](docs/design/cli-agent-workflow.md) | The CLI defects found by driving four agents from a shell, with the ones still open |
| [CHANGELOG.md](CHANGELOG.md) | Release notes and migration between versions |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Commit format, code style, testing, where a fact lives |

Every document under `docs/` is named above or in a document that links to it, and a test
fails on an orphan, so a new file is registered rather than left to be found.

## Configuration

`agents.json` lives in a `.pu/` directory — `./.pu/agents.json` (project) or
`~/.pu/agents.json` (user). Every field is in
[docs/configuration.md](docs/configuration.md#agentsjson):

```json
{
  "name": "local-assistant",
  "backend": { "gateway": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:2b" },
  "security": { "sandbox_root": ".", "forbidden_patterns": ["cd", "rm -rf", "X"] }
}
```

`gateway` is required and must name a product the build carries. A config that
names a protocol instead is refused at load rather than guessed at.

The turn can reach a directory that is not this workspace's own with `ask_dir`, and
`delegates` names who may be asked by name. Both are in
[docs/configuration.md](docs/configuration.md).

**Upgrading** — a workspace written by an older version may need its
`agents.json` edited. [docs/upgrading.md](docs/upgrading.md) lists, version by version,
which keys moved and what a session file saved by an older build does; 0.7.2 renamed
`nim` to `nvidia` and `codebuddy` to `tencent`, with no aliases kept.

Details of each release are in [CHANGELOG.md](CHANGELOG.md).

## Environment Variables

The full table is in [docs/configuration.md](docs/configuration.md#environment-variables).
The ones a normal run touches:

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Name an `agents.json` outright, ahead of both default locations |
| `PU_WEB_DIR` | The page directory `pu serve` serves (defaults beside the binary, then `./web`) |
| `PU_SERVE_BIND` / `PU_SERVE_PORT` | Bind interface and port for `pu serve` |
| `PU_LOG_LEVEL` | File log level, default `info` |

## Web Server (`pu serve`)

`pu serve` puts a local web UI on top of the same single-session runtime:

```bash
./build/pu serve                       # listen on 127.0.0.1:8080
./build/pu serve --bind 0.0.0.0 --port 9000
```

A server serves the directory it was started in and stays there; each directory is a
session with its own configuration, conversation and tools. The workspace can name
its own bind under `serve` in `agents.json`.

> **Warning** — the server has no authentication and no origin checks, so anyone
> who can reach the port can read the session history and run the active agent's
> tools. Keep the default loopback bind unless the port is protected by other means.

The WebSocket protocol, the REST endpoints and the single-page rule are in
[docs/web-api.md](docs/web-api.md).

## License

GPL-3.0 — see [LICENSE](LICENSE)
