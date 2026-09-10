# Contributing

## Pull Requests
- Title format: `type: short description` (e.g. `feat: add tool to ...`)
- Include `## Description`, `## Why`, `## Related Issue` in body.

## Commit Messages
- Follow `type: description` (imperative mood, <72 chars).

## Code Style
- C++23 with Google C++ Style.
- SPDX license header in every file (`// SPDX-License-Identifier: GPL-3.0-only`).
- No decorative comments (`// ====`).
- Comments explain **why**, not **what**.
- Use `clang-format` for formatting; the config lives in `.clang-format`
  (Google style, 100-column limit). Run `clang-format -i <files>` before
  committing. Without the config file, `clang-format` would fall back to the
  LLVM defaults and reformat the whole tree, so always run it from the repo
  root.
- JSON code uses Boost.JSON (`boost::json::value`) through the
  `include/pu/core/json.hpp` helpers (`pu::json::parse`, `pu::json::serialize`,
  `pu::json::ValueOrDefault`, `pu::json::HasKey`, `pu::json::Merge`,
  `pu::json::PrettyPrint`) instead of raw hand-rolled parsing.

## Build Dependencies

Build/test requirements match `README.md`; the key C++ dependencies are
**Boost** (>= 1.75) with the `system`, `program_options`, `json`, and
`beast`/`asio` components, **spdlog**, and **OpenSSL**.

- **Linux (Debian/Ubuntu)**

  ```bash
  sudo apt-get install -y libboost-system-dev libboost-program-options-dev \
      libboost-json-dev libspdlog-dev libssl-dev catch2
  # or: sudo apt-get install -y libboost-all-dev
  ```

- **macOS**

  ```bash
  brew install boost spdlog openssl catch2
  ```

- **Windows (vcpkg)**

  ```bash
  vcpkg install boost-system boost-program-options boost-json spdlog openssl catch2
  ```

## Testing
Run before submitting:
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Key Test Areas
- Workspace: Transcript, Memory.
- LLMProvider: Ollama/OpenAI request building, streaming, tool calling.

## Logging

- The console only shows `error` and `critical` messages; `info`, `warn`, `debug`, and `trace` are never printed to the console.
- Set `PU_LOG_LEVEL` to `trace`, `debug`, `info`, `warn`, `error`, or `critical` to control the file log verbosity (default `info`).
- Log files are stored in `<data-dir>/logs/pu.log` (rotated, max 5MB per file, 3 files kept).
- The data directory is `PU_HOME` if set, otherwise `./.pu/`.

## Directory Structure

```text
src/
  app/           main(), CLI parsing, `pu serve` web server
                 (serve.cpp = lifecycle, serve_http_routes.cpp = REST/static,
                  serve_websocket.cpp = /ws protocol)
  core/          Base layer: logging, platform probing
  infra/         Adapters: Beast HTTP client implementation
  llm/           Providers (Ollama, OpenAI, streaming parser)
  mcp/           MCP transport implementations, JSON-RPC client, high-level client
  session/       Session, Workspace, Transcript, Memory
  tools/         Toolbox, built-in tools, McpTool adapter
  *.cpp          Orchestration layer: agent_config, agent_manager,
                 executor, runtime, command_router
include/pu/
  core/          Base utilities: cancel_token, error, json, path_utils,
                 logging, platform
  infra/         http_client interface + beast_http_client header
  llm/           Public headers for the llm layer
  mcp/           Public headers for the mcp layer (transport, stdio_transport, ...)
  session/       Public headers for the session layer
  tools/         Public headers for the tools layer
  *.hpp          Orchestration-layer headers (agent_config, executor, ...)
tests/unit/      Unit tests
tests/mocks/     Test doubles
```

A header belongs in `include/pu/` when code outside its own directory uses it
(including tests); otherwise it stays next to its `.cpp` under `src/`.

The layering is: `core/` (no dependencies, no domain knowledge) →
`infra/`, `session/`, `llm/`, `mcp/`, `tools/` (domain modules) →
orchestration headers at the root of `include/pu/`.

### CMake Targets

- `pu_core` — base static library: `core/`, `infra/`, `llm/`, `mcp/`, `session/`.
- `pu_agent` — orchestration layer: `agent_config`, `agent_manager`, `executor`,
  `runtime`, `command_router`, `tools/`.
- `pu_app` — app layer: `src/app/cli.cpp` and the `pu serve` modules. Linked by
  both the `pu` executable and `pu_tests`, so tests never compile app sources
  directly.
- `pu` — executable: `src/app/main.cpp`.

## Configuration

- The configuration file must be located in a `.pu/` directory.
- Search order is `./.pu/agents.json` then `~/.pu/agents.json`.

## Adding Features

- **New backend**: Implement `pu::LLMProvider`, update `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add to `CommandRouter`, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered and registered automatically via the MCP client.

## Web Development

- Front-end sources live in `web/` (`index.html`, `app.js`, `style.css`). They are
  served verbatim by `pu serve` — there is no build step for the UI.
- The browser talks to the runtime through a **WebSocket** for chat (`ws://`) and
  REST endpoints for control/status. The chat API is **not** SSE‑based.
- WebSocket protocol:
  - Client → Server: `{"type":"run","payload":{"text":"..."}}` or `{"type":"cancel"}`
  - Server → Client: `{"type":"chunk","payload":{"text":"..."}}`, `{"type":"done"}`, or `{"type":"error","payload":{"text":"..."}}`
- REST endpoints:
  - `GET /api/session` – current session info
  - `GET /api/history` – full conversation history
  - `GET /api/agents` – list available agents
  - `POST /api/agent/switch` – switch agent (`{"agent_name":"..."}`)
  - `GET /api/workspaces` – list workspaces
  - `POST /api/workspace/switch` – switch workspace (`{"path":"..."}`)
  - `POST /api/clear` – clear history
- After editing C++ or any file under `web/`, rebuild (`cmake --build build`) and
  restart `pu serve`; the server mounts `web/` at startup, so a plain restart is
  enough to pick up front-end changes.
- In a browser, verify the typewriter (streaming) output, the Send→Cancel button,
  agent switching from the dropdown, and history loading on refresh.

## License

GPL-3.0 — see [LICENSE](LICENSE)
