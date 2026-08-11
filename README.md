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

Create `agents.json` in current directory or `~/.pu/`, or let the interactive wizard scaffold it for you:

```bash
pu config add
```

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
pu chat
> /backend deepseek-pro    # switch to predefined agent
```

```bash
pu config list           # list agents
pu config add            # add an agent (interactive wizard)
pu config refresh-models # scan providers for available models
pu config probe          # system and provider status
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

## Agent Configuration CLI (`pu config`)

Manage `agents.json` without editing it by hand:

| Subcommand | Description |
|------------|-------------|
| `list` | List all agents |
| `show <name>` | Show agent details |
| `add [name]` | Add a new agent (interactive wizard) |
| `remove <name>` | Remove an agent |
| `rename <old> <new>` | Rename an agent |
| `set-default <name>` | Set the default agent |
| `refresh-models` | Scan providers (`nim`, `ollama`, `openai`) for available models |
| `probe` | Show system and provider status |

Options:

- `--json` — machine-readable output for `list`, `show`, `refresh-models`, `probe`.
- `--provider=<name>` — restrict `refresh-models` to a single provider.
- `-h`, `--help` — show usage.

`refresh-models` uses `NVIDIA_API_KEY` and `OPENAI_API_KEY` when present, and
reaches Ollama at `http://localhost:11434`. The scanned model list is also used
by the `add` wizard to suggest available models.

---

## Tools & Agent Binding

Tool set is bound to the active agent – switching agents with `/backend <agent>` automatically rebuilds the registry (stops previous MCP servers, starts new ones).

- `tools` in `agents.json` is a whitelist for built-in tools (`execute_bash`,
  `write_file`); an empty list enables all. `ask_user` is always available.
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

This allows the executor to distinguish success from failure and provide clear feedback to the model. The full JSON result is stored as the tool message in the transcript, so the model sees the exact outcome on later turns.

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
| `NVIDIA_API_KEY` | API key for NVIDIA NIM (`pu config refresh-models`) |
| `OPENAI_API_KEY` | API key for OpenAI-compatible endpoints (`pu config refresh-models`) |

### Logging

Log files stored in `~/.pu/logs/pu.log` (rotated, max 5MB per file, 3 files kept).

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
