# pu-cli

> “朴散则为器”——《老子》

A minimalist, extensible CLI orchestrator for large language models.
Chat with experts, run system commands, and extend with custom experts.

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
Create `experts.json` in the project or current directory:
```json
{
  "default_expert": "chat",
  "experts": [
    {
      "name": "chat",
      "type": "chat",
      "description": "Local Ollama chat",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:2b",
        "temperature": 0.7,
        "system_prompt": "You are a helpful assistant."
      }
    },
    {
      "name": "bash",
      "type": "bash",
      "description": "Safe command executor",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:2b",
        "temperature": 0.0
      },
      "executor": {
        "sandbox": "."
      }
    }
  ]
}
```
Environment variables with `${VAR}` syntax are expanded automatically.

## Usage

```bash
# Single prompt (uses default expert or auto-routing)
./build/pu ask "Explain quantum computing in one sentence"

# Interactive chat with a specific expert
./build/pu chat --expert bash

# In chat mode, use @expert to address any expert directly
> @bash list files in current directory
```

In chat mode, type `/help` to see built‑in commands:
- `/expert <name>` – switch the active expert
- `/experts` – list all configured experts
- `/clear` – reset session and expert lock

## Testing
```bash
cmake --build build --target test
# or
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).
