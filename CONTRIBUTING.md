# Contributing

## Documentation

For architecture overview and design decisions, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Pull Requests
- Title format: `type: short description` (e.g. `feat: add tool calling to BashExpert`)
- Include a bullet list of changes in the PR body
- Use the sections `## Description`, `## Why`, `## Related Issue` (write N/A if none)

## Commit Messages
- Follow the same `type: description` convention
- Keep the first line under 72 characters
- Use imperative mood (e.g. `fix:` not `fixed:`)

## Code Style
- C++23 with Google C++ Style
- SPDX license identifier (`// SPDX-License-Identifier: GPL-3.0-only`) in every source file
- Remove decorative separator comments and self‑explanatory variable comments
- Use `clang-format` for consistent formatting

### Comments & Self-Documenting Code
- **Code should be self-explanatory**: name functions and variables clearly.
- Comments should explain **why**, not **what**.
- Avoid decorative separators (e.g., `// ====`, `// -----`).
- Use English for all comments.
- When in doubt, write clearer code instead of adding comments.

## Testing
Run tests before submitting:
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Module Testing Requirements

### core::Context
- Test parent-child isolation (child can read parent, but not vice versa)
- Test SetVar/GetVar roundtrip for all JSON types
- Test Save/Load persistence

### DelegationStack
- Test push/pop with explicit and implicit contexts
- Test depth tracking and max depth enforcement
- Test Clear and GetRootContext

## Directory Structure
```
src/
  agent/         Agent lifecycle, factory, executor, tool registry
  app/           CLI, UI, session manager, renderer
  backends/      LLM backend implementations (Ollama, OpenAI)
  conversation/  Conversation store (save/load/export)
  infra/         HTTP client, platform utilities
  runtime/       Context, delegation stack, orchestrator, global context
  tools/         Built-in tools, command executor, Python tool
include/pu/      Public headers
tests/unit/      Unit tests
```

## Adding New Agents
- Built-in agents are defined in `agents.json`.
- Agent definitions follow JSON schema with `name`, `description`, `prompt_layers`, `input_schema`, `output_schema`.

## Learning Module
- `pu learn` analyzes conversations in `~/.pu/conversations/`.
- To add a success marker, a conversation can be rated or marked via a future command.
- Generated agents appear in `~/.pu/generated/agents/` and can be reviewed before loading.