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

For architecture details, CLI command reference, and configuration guide,
see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing
```bash
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).