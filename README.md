# pu-cli

> “朴散则为器”——《老子》

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
Create `experts.json` in the project or current directory (same format as before, see [examples/](examples/)).

## Usage

### Interactive chat with agent stack
```bash
./build/pu chat --expert chat
```

**Stack commands** (experimental):
- `/push <agent>` – Push a new agent onto the call stack
- `/pop` – Pop the current agent
- `/stack` – Show current call stack

**Memory commands**:
- `/note add <text>` – Add a note for current expert
- `/note show` – Show notes for current expert
- `/save [name]` – Save conversation and generate summary (stored in global context)

### Learning from conversations
```bash
./build/pu learn --threshold 0.6 --max-sessions 10
```
Analyzes successful conversations and generates new agent definitions in `~/.pu/generated/agents/`.

## Architecture

- **GlobalContext**: shared structured memory (tape) with JSON storage and automatic persistence.
- **CallStack**: stack frames for nested agent calls, supporting PUSH/POP/HALT.
- **Orchestrator**: executes agents based on stack top, handles pending actions (push/pop) requested by agents.
- **Agent** (renamed from Expert): each agent is a Turing machine state with its own system prompt and capabilities.
- **Learning module**: offline analysis to generate new agents from successful traces.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

## Testing
```bash
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).
