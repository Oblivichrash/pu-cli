# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **multi-session isolation**, **Git-style fork-merge branching**, and **dynamic backend switching**.

---

## Quick Start

### Build

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Configure

Create `agents.json` in current directory or `~/.pu/`:

```json
{
  "default_agent": "local-assistant",
  "agents": [
    {
      "name": "local-assistant",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:2b"
      },
      "tools": ["execute_bash", "write_file"]
    }
  ]
}
```

### Usage

```bash
./build/pu chat
> /backend deepseek-pro    # switch to predefined agent
> /fork experiment         # create branch
> /merge                   # merge back
```

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/backend <agent>` | Switch to predefined agent |
| `/backend <type> <model>` | Manual backend switch |
| `/fork [<agent>]` | Fork new branch |
| `/fork list` | Show branch tree |
| `/merge` | Merge current branch |
| `/save [name]` | Save session |
| `/load <id>` | Load session |
| `/list` | List saved sessions |
| `/clear` | Clear history |
| `/exit` | Exit |

---

## Configuration

### `agents.json`

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:4b",
        "temperature": 0.7,
        "max_tokens": 4096
      },
      "tools": ["execute_bash", "write_file"],
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["rm -rf", "sudo"]
      }
    }
  ]
}
```

### Environment

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Path to agents.json |
| `PU_HOME` | Data directory (default `~/.pu/`) |

### Logging

- Set `PU_LOG_LEVEL` to `trace`, `debug`, `info`, `warn`, `error`, or `critical`.
- Set `PU_TRACE=1` to enable trace-level logging (equivalent to `PU_LOG_LEVEL=trace`).
- Log files are stored in `~/.pu/logs/pu.log` (rotated, max 5MB per file, 3 files kept).

---

## License

GPL-3.0 — see [LICENSE](LICENSE)