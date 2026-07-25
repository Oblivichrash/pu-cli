# pu-cli

> "朴散则为器"——《老子》

A minimalist, extensible CLI orchestrator for large language models, now with **Turing-machine-like multi-agent system** supporting nested subtasks, shared context, and self-learning.

## Quick Start

### Install dependencies
```bash
# Ubuntu
sudo apt install libcurl4-openssl-dev nlohmann-json3-dev catch2

# macOS
brew install curl nlohmann-json catch2

# Windows (vcpkg)
vcpkg install curl nlohmann-json catch2
```

### Build
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### Configure
Create `agents.json` in the project or current directory (see [examples/](examples/)).

## Usage

### Interactive chat with agent stack
```bash
./build/pu chat --agent chat
```

**Stack commands**:
- `/push <agent>` – Push a new agent onto the delegation stack
- `/pop` – Pop the current agent from the stack
- `/stack` – Show current delegation stack

**Memory commands**:
- `/note add <text>` – Add a note for current agent
- `/note show` – Show notes for current agent
- `/save [name]` – Save conversation and persist context

### Learning from conversations
```bash
./build/pu learn --threshold 0.6 --max-sessions 10
```
Analyzes successful conversations and generates new agent definitions in `~/.pu/generated/agents/`.

## Architecture

- **core::Context**: Hierarchical key-value store with parent-child isolation, used for structured memory and delegation scoping.
- **DelegationStack**: Stack-based delegation model supporting PUSH/POP for nested agent calls with isolated or shared contexts.
- **Orchestrator**: Executes agents based on stack top, handles pending actions (push/pop) requested by agents.
- **AgentManager**: Manages agent lifecycle, configuration loading, and state snapshots.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

### Telemetry
Set `PU_TRACE=1` to enable detailed execution traces for debugging delegation and tool calls.

## Testing
```bash
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).