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
- Workspace: Fork/Merge, Transcript, Memory.
- CallStack: Push/Pop, depth tracking.
- ForkMergeService: Fork, Merge, PrintTree, Prune.
- LLMProvider: Ollama/OpenAI request building, streaming, tool calling.

## Directory Structure

```
src/
  agent/         AgentManager (config metadata only)
  app/           CLI, UI, session manager
  core/          ForkMergeService, SummaryGenerator, FactExtractor
  executor/      Executor (stateless)
  infra/         HTTP client, platform utils
  llm/           Providers
  runtime/       Runtime, CommandRouter
  session/       Session, Workspace, Transcript, Memory, RevisionGraph, CallStack
  storage/       SessionStore
  tools/         Toolbox, tools
include/pu/      Public headers
tests/unit/      Unit tests
```

## Adding Features

- **New backend**: Implement `pu::LLMProvider`, update `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::Initialize()`.
- **New command**: Add to `CommandRouter`, update help.

## License

GPL-3.0 — see [LICENSE](LICENSE)
