# pu-cli

> "朴散则为器"——《老子》

A minimalist, extensible CLI orchestrator for large language models, with a **Git-inspired fork-merge memory system** supporting nested subtasks, shared context, and tool-based execution.

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
Create `agents.json` in the project or current directory (see examples/).

## Usage

### Interactive chat with agent stack
```bash
./build/pu chat --agent chat
```

### Commands

| Command | Description |
|---------|-------------|
| `/fork [agent]` | Create isolated branch (uses current agent if omitted) |
| `/explore <goal>` | Execute task on current branch (multi-turn) |
| `/merge` | Interactive merge strategy selection |
| `/merge --full` | Merge with full history |
| `/fork list` | Show ASCII branch tree |
| `/fork show <id>` | Show detailed branch info |
| `/fork prune` | Preview merged branches |
| `/fork prune --yes` | Remove merged branches |
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/agent <name>` | Switch to a different agent |
| `/save [name]` | Save conversation |
| `/load <id>` | Load conversation |
| `/list` | List saved conversations |
| `/exit` | Exit pu chat |

For architecture details, CLI command reference, and configuration guide,
see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing
```bash
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).
