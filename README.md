# pu-cli

> “朴散则为器”——《老子》

A minimalist, extensible CLI orchestrator for large language models.
Chat with models, run system commands, and extend with custom experts.

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
Create `models.json` in the project or current directory:
```json
{
  "default_model": "local",
  "models": [
    {
      "name": "local",
      "description": "Local Ollama",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:2b"
      }
    }
  ]
}
```
Environment variables with `${VAR}` syntax are expanded automatically.

## Usage

```bash
# Single prompt
./build/pu ask "Explain quantum computing in one sentence"

# Interactive chat
./build/pu chat
./build/pu chat -m gpt --expert bash
```

In chat mode, type `/help` to see built‑in commands.
The `--expert` flag directly activates an expert (e.g. `bash` for safe command execution).

## Testing
```bash
cmake --build build --target test
# or
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).
