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

## Core Commands

| Command | What it does |
| --- | --- |
| `pu ask [--agent NAME] "..."` | One turn in the workspace's conversation, printed to stdout |
| `pu chat [--agent NAME]` | The same conversation, kept open, with `/commands` |
| `pu agents [--json]` | The configured agents, and which one answers |
| `pu models [--agent NAME] [--verify]` | What the agent's gateway reports, and which model is active |
| `pu probe` | A gateway's raw stream, kept, and the field names it actually sent |
| `pu check [--config PATH] [--entry MODEL] [--caps]` | What an `agents.json` resolves to, without contacting a provider |
| `pu serve` | The Web UI and its WebSocket |
| `pu sse URL [--format text\|json\|raw]` | Read a `text/event-stream` from any endpoint and print its events |

Every command exits with one of five statuses and writes only its answer to stdout; both are
in [ARCHITECTURE.md](docs/ARCHITECTURE.md#what-a-command-exits-with-and-what-it-prints).
`pu --help` names every command with the one line that says what it answers.
`pu <command> --help` prints that command's own page.

## Docs

| Document | What it holds |
|----------|---------------|
| [docs/configuration.md](docs/configuration.md) | `agents.json`, `peers.json`, environment variables, logging |
| [docs/web-api.md](docs/web-api.md) | `pu serve`: the WebSocket protocol and the REST endpoints |
| [docs/web-ui.md](docs/web-ui.md) | The browser modules, and how the page is served |
| [docs/paging.md](docs/paging.md) | `execute_bash` page by page: the envelope, cursors, and what a turn keeps |
| [docs/peers-and-board.md](docs/peers-and-board.md) | Peer messaging, the task board, and their verbs |
| [docs/security.md](docs/security.md) | The trust boundary, the peer token, loopback-only routes, sandbox and approval patterns |
| [docs/providers.md](docs/providers.md) | What each gateway puts on the wire |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, directory layout, CMake targets, extension points |
| [docs/releases.md](docs/releases.md) | How a release is cut, and what a CHANGELOG entry is |
| [docs/design/next.md](docs/design/next.md) | What is left to build, in order, and what was decided against |
| [docs/design/bash-output-window.md](docs/design/bash-output-window.md) | The note paging shipped from: one window per tool call, and why |
| [docs/design/gateway-protocol-layout.md](docs/design/gateway-protocol-layout.md) | Why a gateway is a table row rather than a file |
| [docs/design/codebuddy-cloud-api.md](docs/design/codebuddy-cloud-api.md) | One gateway's wire format, and the two refusals it can answer |
| [docs/design/cli-agent-workflow.md](docs/design/cli-agent-workflow.md) | What driving four agents from the command line broke, and what was built for it |
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
  "default_agent": "local-assistant",
  "agents": [
    {
      "name": "local-assistant",
      "backend": { "gateway": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:2b" },
      "security": { "sandbox_root": ".", "forbidden_patterns": ["cd", "rm -rf", "sudo"] }
    }
  ]
}
```

`gateway` is required and must name a product the build carries. A config that
names a protocol instead is refused at load rather than guessed at.

**Upgrading** — the keys that changed, and what a session saved by an older
version does:

| From | What to do |
|------|------------|
| 0.4.x | Rename `backend.type` to `backend.gateway`. The agent-level `"type": "llm"` is a different field and needs no change. |
| 0.5.1 | A cancelled turn now stores its unrun tool calls as `interrupted`. A session saved earlier that still holds a `pending` call keeps refusing `/rewind`; start a new session. |
| 0.6.0 | `pu sse` takes `--format text\|json\|raw` instead of `--json`/`--raw`. `pu models --entry` and `pu models --caps` moved to `pu check --entry` and `pu check --caps`; no configuration change. |
| 0.6.1 | `pu serve --host` is `--bind`, and `PU_SERVE_HOST` is `PU_SERVE_BIND`; `serve` in `agents.json` takes `bind`. `GET /api/history` answers `{"messages": [...]}` rather than an array. `GET /api/servers` lists the other servers as `{"name", "port"}`, with no `directory`, `host` or `current`. |

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
