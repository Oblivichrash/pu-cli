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
include/pu/      Public headers
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

## License

GPL-3.0 — see [LICENSE](LICENSE)
