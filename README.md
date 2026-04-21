# pu-cli

> “朴散则为器”——《老子》

A minimalist, extensible CLI orchestrator for large language models, embracing the Tao of simplicity.

## Status
Under active development – basic skeleton.

## Build

### Prerequisites
- CMake 3.16+
- libcurl
- nlohmann/json (3.10+)
- Catch2 (3.x, for tests)

### Compile
```bash
cmake -B build
cmake --build build
./build/pu
```

### Optional: Generate compile_commands.json for clangd/IDE
```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

## License
GPL-3.0 – see [LICENSE](./LICENSE).
