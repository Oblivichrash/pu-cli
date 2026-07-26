# pu-cli Architecture

> "朴散则为器"——《老子》

pu-cli is a minimalist, extensible CLI orchestrator for large language models with a Git-inspired fork-merge memory system.

## Core Concepts

### Context
Each agent operates within a `core::Context`. It contains:
- **History**: Linear message history for the current branch
- **Variables**: Key-value store for working memory
- **Facts**: Extracted knowledge units for delegation

Contexts form a **tree structure** (Git DAG model):
- Parent-child relationships via `parent_` and `children_`
- Each context has a `branch_name` and `state` (Active, Merged, Abandoned)
- Merge contexts record `merge_parents_` for full history

### Fork-Merge (Git-Style Branching)
- **Fork**: Create an isolated child context (independent history)
- **Explore**: Execute tasks within a branch (multi-turn)
- **Merge**: Integrate branch back (squash or full history)
- **Prune**: Remove merged branches to keep the tree clean

This model solves **token explosion**: sub-task tool outputs stay in the child context; only summaries enter the parent.

### Delegation Stack
Manages nested agent calls:
- `Push`: Start a sub-task
- `Pop`: Complete and merge back
- Max depth: 5 (configurable)

### ForkMergeService
Central service for fork/merge operations, extracted from the Orchestrator:
- `Fork()`: Create a new child context
- `Merge()`: Merge a child context back to parent
- `PrintTree()`: Display the fork tree
- `FindContext()`: Look up a context by ID or branch name
- `PruneMerged()`: Remove merged branches
- `ExtractFacts()`: Extract facts from context history
- `GenerateSummary()`: Generate LLM summary for squash merges
- `PopDelegation()`: Pop and summarize a delegation

### CommandHandler
Handles all slash-commands (`/fork`, `/push`, `/pop`, `/stack`), extracted from the Orchestrator to reduce its responsibilities.

### SummaryGenerator
Handles LLM-based summary generation for squash merges, extracted from ForkMergeService.

### FactExtractor
Handles rule-based fact extraction (regex matching for file paths, error messages), extracted from ForkMergeService.

### Orchestrator
Central execution engine:
- Coordinates between CLI, agents, and core services
- Manages `DelegationStack`
- Routes tasks to appropriate agents
- Delegates fork/merge commands to `CommandHandler` and `ForkMergeService`

### Agents
Agents are defined in `agents.json`:
- `backend`: LLM configuration (ollama/openai)
- `tools`: Available tools
- `security`: Sandbox policies

## CLI Commands

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

## Configuration

### agents.json

```json
{
  "default_agent": "chat",
  "agents": [
    {
      "name": "chat",
      "description": "Main assistant",
      "backend": {
        "type": "ollama",
        "host": "http://localhost:11434",
        "model": "qwen3.5:4b"
      },
      "tools": ["execute_bash", "write_file", "fork_context", "merge_context"]
    },
    {
      "name": "bash-expert",
      "description": "Bash exploration specialist",
      "backend": { "type": "ollama", "model": "qwen3.5:4b", "host": "http://localhost:11434" },
      "tools": ["execute_bash"]
    }
  ]
}
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `PU_AGENTS_CONFIG` | Path to agents.json |
| `PU_TRACE` | Enable tracing (1 = metadata, 2 = full data) |

## Directory Structure

```
src/
  agent/         Agent lifecycle, factory, executor, tool registry
  app/           CLI, UI, session manager, renderer
  backends/      LLM backend implementations (Ollama, OpenAI)
  conversation/  Conversation store (save/load/export)
  core/          ForkMergeService, CommandHandler, SummaryGenerator, FactExtractor
  infra/         HTTP client, platform utilities
  runtime/       Context, delegation stack, orchestrator
  tools/         Built-in tools, command executor, Python tool
include/pu/      Public headers
tests/           Unit tests
```

## Core Components

### ForkMergeService (`src/core/fork_merge_service.cpp`)
Extracted from Orchestrator, this service contains all fork/merge logic:
- Creates child contexts (forks)
- Merges contexts back to parents (squash or full)
- Prints the fork tree
- Finds contexts by ID or branch name
- Prunes merged branches

### CommandHandler (`src/core/command_handler.cpp`)
Extracted from Orchestrator, handles slash-commands:
- `/fork`, `/fork list`, `/fork show`, `/fork prune`
- `/push` (deprecated, delegates to fork)
- `/pop` (deprecated, delegates to merge)
- `/stack`

### SummaryGenerator (`src/core/summary_generator.cpp`)
Handles LLM-based summary generation:
- Used by ForkMergeService for squash merges
- Calls the agent's LLM to generate concise summaries

### FactExtractor (`src/core/fact_extractor.cpp`)
Rule-based fact extraction:
- Regex matching for file paths
- Error/failure message detection
- Deduplication of facts

## Tool System

Tools are C++ classes implementing `agent::Tool` interface. The fork-related tools
(`fork_context`, `merge_context`, `list_forks`) now use `ForkMergeService` to
perform real operations instead of returning placeholder values.

## Extension Guide

### Adding a New Agent
1. Add entry to `agents.json`
2. Define backend (type, host, model)
3. Specify tools
4. Set security policies (optional)

### Adding a New Tool
1. Create `include/pu/tools/your_tool.hpp`
2. Create `src/tools/your_tool.cpp`
3. Implement `agent::Tool` interface
4. Register in `AgentRegistry::CreateAgent()`

## Telemetry

Set `PU_TRACE=1` to enable execution traces for debugging delegation and tool calls.

## Testing

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

GPL-3.0 – see [LICENSE](../LICENSE)
