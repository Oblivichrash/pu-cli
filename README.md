# pu-cli

> "朴散则为器"——《老子》

A minimalist CLI orchestrator for LLMs with **multi-session isolation** and **dynamic backend switching**.

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
      "tools": ["execute_bash", "write_file"],
      "security": {
        "sandbox_root": ".",
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

### Usage

```bash
./build/pu chat
> /backend deepseek-pro    # switch to predefined agent
```

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/backend <agent>` | Switch to predefined agent (rebuilds tool set) |
| `/backend <type> <model>` | Manual backend switch |
| `/agents` | List available agents |
| `/save [name]` | Save conversation |
| `/load <id>` | Load conversation |
| `/list` | List saved conversations |
| `/note add <text> \| /note show` | Add or show notes |
| `/clear` | Clear conversation history |
| `/exit`, `/quit` | Exit |

---

## Tools & Agent Binding

Tool set is bound to the active agent – switching agents with `/backend <agent>` automatically rebuilds the registry (stops previous MCP servers, starts new ones).

- `tools` in `agents.json` filters built‑in tools.
- MCP tools are exposed with a `mcp.<server>.<tool>` prefix.

### Tool Output Format

All tools now return structured JSON with the following fields:

```json
{
  "success": true/false,
  "stdout": "...",
  "stderr": "...",
  "error": "...",
  "exit_code": 0
}
```

This allows the executor to distinguish success from failure and provide clear feedback to the model. The transcript stores the extracted `stdout` or `error` content; the full JSON is not persisted.

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
        "forbidden_patterns": ["cd", "rm -rf", "sudo"]
      }
    }
  ]
}
```

### Security

- `forbidden_patterns` – commands containing these substrings are blocked.
- `sandbox_root` – all file operations are relative to this directory.
- It is strongly recommended to include `"cd"` in `forbidden_patterns` to prevent the model from changing the working directory, which can cause confusion.

### MCP Servers

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "backend": { "type": "ollama", "host": "http://localhost:11434", "model": "qwen3.5:4b" },
      "tools": ["execute_bash", "write_file"],
      "mcp_servers": [
        {
          "name": "filesystem",
          "command": "npx",
          "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
        }
      ]
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `name` | Display name for the MCP server |
| `command` | Executable to launch |
| `args` | Arguments passed to the executable |

### Thinking Mode & History Compaction

```json
{
  "default_agent": "deepseek",
  "agents": [
    {
      "name": "deepseek",
      "backend": {
        "type": "openai",
        "host": "https://api.deepseek.com/v1",
        "model": "deepseek-reasoner",
        "api_key": "${DEEPSEEK_API_KEY}",
        "enable_thinking": true,
        "temperature": 0.1
      },
      "history_compaction": {
        "enabled": false,
        "keep_head": 15,
        "keep_tail": 60
      }
    }
  ]
}
```

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `PU_AGENTS_CONFIG` | Path to agents.json |
| `PU_HOME` | Data directory (default `~/.pu/`) |
| `PU_LOG_LEVEL` | Log level: `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `PU_LOG_JSON=1` | Enable structured JSON logging |
| `PU_LOG_CONSOLE=0` | Disable console logging |

### Logging

Log files stored in `~/.pu/logs/pu.log` (rotated, max 5MB per file, 3 files kept).

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
