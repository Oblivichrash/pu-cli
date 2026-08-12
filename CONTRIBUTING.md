# Contributing

## Pull Requests
- Title format: `type: short description` (e.g. `feat: add tool to ...`)
- Include `## Description`, `## Why`, `## Related Issue` in body.

## Commit Messages
- Follow `type: description` (imperative mood, <72 chars).
- Use a bullet list in the body to summarize key changes (`- Add ...`), matching existing history.

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

- Set `PU_LOG_LEVEL` to `trace`, `debug`, `info`, `warn`, `error`, or `critical`.
- Log files are stored in `~/.pu/logs/pu.log` (rotated, max 5MB per file, 3 files kept).

## Directory Structure

```
src/
  main.cpp, cli.cpp  CLI entry, chat UI
  config_tools/      `pu config`: agent CRUD, wizard, model scanner, system probe
  core/              Logging
  infra/             CurlHttpClient, platform utils
  llm/               Providers, streaming parser
  mcp/               MCP transport, JSON-RPC client, high-level client
  session/           Session, Workspace, Transcript, Memory
  tools/             Toolbox, built-in tools, MCP tool adapter
  agent_config.cpp, agent_manager.cpp, command_router.cpp, config_cli.cpp,
  executor.cpp, runtime.cpp, session_store.cpp
include/pu/      Public headers
tests/unit/      Unit tests
```

## Adding Features

- **New backend**: Implement `pu::LLMProvider`, update `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add to `CommandRouter`, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered and registered automatically via the MCP client.

## License

GPL-3.0 — see [LICENSE](LICENSE)
