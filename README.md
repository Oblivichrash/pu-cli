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
      "tools": ["execute_bash", "write_file"]
    }
  ]
}
```

### Usage

```bash
./build/pu chat
> /backend deepseek-pro    # switch to predefined agent (rebuilds tools)
```

---

## Core Commands

| Command | Description |
|---------|-------------|
| `/backend <agent>` | Switch to predefined agent (rebuilds the active tool set) |
| `/backend <type> <model>` | Manual backend switch (keeps current agent) |
| `/save [name]` | Save session |
| `/load <id>` | Load session |
| `/list` | List saved sessions |
| `/clear` | Clear history |
| `/exit` | Exit |

---

## Tools & Agent Binding

The tool set is bound to the active agent – switching agents with `/backend <agent>` automatically rebuilds the registry (stops previous MCP servers, starts new ones). No manual reload needed.

- `tools` in `agents.json` filters which built-in tools are advertised to the model.
- MCP tools are always exposed with a `mcp.` prefix when a server is configured.

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

### MCP Servers

Add `mcp_servers` to any agent to connect external tools via the [Model Context Protocol](https://modelcontextprotocol.io/). Servers are started as child processes (stdio transport) and their tools are registered with a `mcp.` prefix in the active agent's tool set. **Each agent can define its own servers**; switching to that agent starts them, switching away stops them.

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

> Note: only the first `mcp_servers` entry per agent is currently started.

### Thinking Mode & History Compaction

Backends targeting DeepSeek/vLLM reasoning models can enable thinking mode via `enable_thinking`. When enabled, the provider streams `reasoning_content` alongside the answer.

History compaction trims the middle of the conversation to save tokens, keeping `keep_head` leading and `keep_tail` trailing messages. **It is skipped automatically when the backend is in thinking mode** (compaction would drop the intermediate `reasoning_content` and cause API errors); a warning is logged and the model receives the full history instead.

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
      "tools": ["execute_bash", "write_file"],
      "security": { "sandbox_root": "." },
      "history_compaction": {
        "enabled": false,
        "keep_head": 15,
        "keep_tail": 60,
        "strategy": "truncate"
      }
    }
  ]
}
```

| Field | Description | Default |
|-------|-------------|---------|
| `enable_thinking` | Enable thinking mode (DeepSeek/vLLM only) | `true` |
| `history_compaction.enabled` | Enable compaction when the provider is not in thinking mode | `true` |
| `history_compaction.keep_head` | Number of leading messages to keep | `10` |
| `history_compaction.keep_tail` | Number of trailing messages to keep | `50` |
| `history_compaction.strategy` | Compaction strategy (reserved; only `truncate` today) | `"truncate"` |

For thinking-mode backends set `enable_thinking: true` and `history_compaction.enabled: false` for clarity (the runtime skips compaction and warns anyway). For ordinary models (Ollama, non-thinking OpenAI) leave compaction enabled to save tokens.

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
