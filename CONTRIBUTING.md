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
- Use `clang-format` for formatting.
- JSON code uses Boost.JSON (`boost::json::value`) through the
  `include/pu/json.hpp` helpers (`pu::json::parse`, `pu::json::serialize`,
  `pu::json::ValueOrDefault`, `pu::json::HasKey`, `pu::json::Merge`,
  `pu::json::PrettyPrint`) instead of raw hand-rolled parsing.

## Build Dependencies

Build/test requirements match `README.md`; the key C++ dependencies are
Boost (>= 1.75) with the `system`, `program_options`, and `json` components.

- **Linux (Debian/Ubuntu)**

  ```bash
  sudo apt-get install -y libcurl4-openssl-dev libspdlog-dev libcpp-httplib-dev \
      libboost-system-dev libboost-program-options-dev libboost-json-dev catch2
  ```

- **macOS**

  ```bash
  brew install curl catch2 spdlog cpp-httplib boost
  ```

- **Windows (vcpkg)**

  ```bash
  vcpkg install curl catch2 spdlog cpp-httplib boost-system boost-program-options boost-json
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
  agent/         AgentManager (config metadata only)
  app/           CLI, UI, session manager
  core/          SummaryGenerator, ArtifactExtractor
  executor/      Executor (stateless)
  infra/         HTTP client, platform utils
  llm/           Providers
  mcp/           MCP transport, JSON-RPC client, high-level client
  runtime/       Runtime, CommandRouter
  session/       Session, Workspace, Transcript, Memory
  tools/         Toolbox, tools (including McpTool adapter)
include/pu/      Public headers (incl. `pu/json.hpp` Boost.JSON helpers)
tests/unit/      Unit tests
```

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
- The browser talks to the runtime through the API implemented in
  `src/app/serve.cpp` (`RunServe`): `POST /api/chat/stream` (SSE),
  `POST /api/chat` (non-streaming), `POST /api/chat/cancel`,
  `POST /api/agent/switch`, `POST /api/clear`, `GET /api/session`,
  `GET /api/history`, `GET /api/agents`.
- After editing C++ or any file under `web/`, rebuild (`cmake --build build`) and
  restart `pu serve`; the server mounts `web/` at startup, so a plain restart is
  enough to pick up front-end changes.
- Manual checks (server on default port 8080):
  ```bash
  curl -N -X POST http://127.0.0.1:8080/api/chat/stream \
    -H 'Content-Type: application/json' \
    -d '{"message":"hi","request_id":"1"}'
  # expect data: {"token": ...} events followed by data: [DONE]

  curl -X POST http://127.0.0.1:8080/api/chat/cancel \
    -H 'Content-Type: application/json' \
    -d '{"request_id":"1"}'
  # expect {"success":true} while the request is in flight
  ```
- In a browser, verify the typewriter (streaming) output, the Send→Cancel button,
  agent switching from the dropdown, and history loading on refresh.
- The front-end falls back to the non-streaming `POST /api/chat` endpoint when the
  browser lacks `ReadableStream` or the server returns an HTTP error, so keep that
  route working when changing the chat API.

## License

GPL-3.0 — see [LICENSE](LICENSE)
