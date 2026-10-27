# Perception as a plugin, and what "generic" means across platforms

Two questions, one answer: make the **contract** portable and the **readers**
disposable. A plugin boundary is the right shape for this, but not the shape the
word usually implies — no `.so`, no ABI, no Qt. The project already has the pattern
twice (gates are compile-time plugins, MCP servers are runtime ones), and the
perception readers fit the second one, with one exception that decides the design.

**Conclusion up front:** Wayland + Konsole can only ever report *identity* (which
tab, which pid, which cwd, which foreground job) unless the user runs tmux or pu owns
the pty. That ceiling does not change with a plugin system — but a plugin system is
what stops the ceiling from being baked into the core, and it is what makes Windows
and macOS adapters additive rather than invasive.

---

## 1. Why not a shared library

A `.so`/`.dll` plugin looks like the obvious answer and fails on four points this
codebase has already argued about elsewhere:

| Problem | Consequence here |
|---|---|
| C++ ABI | plugins must be rebuilt per compiler and per standard library; the project ships one binary |
| Crash containment | a provider that segfaults on a bad `/proc` read takes the session with it — the same reason `native-library-calls.md` puts FFI in a worker |
| Dependency leak | a Konsole provider wants Qt DBus; linking it drags Qt into a Boost-only CLI |
| Version skew | the plugin and the core disagree about `Snapshot`'s fields, silently |

An out-of-process plugin over stdio has none of those, and the machinery already
exists in this tree: `mcp::StdioTransport` spawns a child, and `JsonRpcClient` frames
messages. A perception provider is a smaller protocol than MCP.

**But one reader cannot be a subprocess**: the pty provider owns a file descriptor
that the core created when it spawned a command. Ownership of a live fd is not
transferable across a process boundary without `SCM_RIGHTS` games that Windows
cannot play. So the seam must be an *interface*, and a subprocess must be one way to
satisfy it — not the only way.

```cpp
namespace pu::percept {

// One field of a snapshot. The core stamps `provenance`; a provider only supplies
// `value`. A provider that could lie about where a fact came from is a provider
// that will.
struct Field {
  boost::json::value value;
  std::string provider;   // stamped by the registry, not by the provider
};

struct Snapshot {
  std::map<std::string, Field> fields;  // cursor, viewport, mouse, cwd, ...
  std::vector<std::string> redactions;
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string Id() const = 0;
  // Declared once. The registry never asks for a field a provider did not claim,
  // and drops a field it did not declare even if it arrives.
  virtual std::vector<std::string> Fields() const = 0;
  virtual std::optional<Snapshot> Read(const Query&) = 0;
};

}  // namespace pu::percept
```

Two implementations: `PtyProvider` (in-process, holds the fd) and
`SubprocessProvider` (generic, spawns any executable that speaks §2). The `tmux`
reader can be either; it is cheaper in-process, but writing it as a 40-line shell
script first is how the field vocabulary gets settled.

## 2. The protocol

Line-delimited JSON on stdin/stdout, one request per line, one response per line.
No ids, no pipelining, no subscriptions — a snapshot is a question, not a feed.

```
-> {"op":"capabilities"}
<- {"protocol":1,"id":"konsole","fields":["active","cwd","foreground"],
    "platforms":["linux"],"cost":"low","needs":["dbus"]}

-> {"op":"snapshot","session":{"tty":"/dev/pts/3","pane":"%1"},
    "budget":{"tokens":200}}
<- {"fields":{
      "active":{"tab":"/Sessions/2","title":"vim src/net.cpp"},
      "cwd":{"path":"/repo/src"},
      "foreground":{"argv":["vim","src/net.cpp"],"pid":4213}},
    "redactions":[]}
```

Rules that make it safe to have many of these:

- **`fields` is a closed vocabulary** owned by the core (`cursor`, `viewport`,
  `mouse`, `selection`, `cwd`, `foreground`, `active_tab`, `grid`, `last_command`).
  A provider may fill a subset; an unknown key is dropped with a warning, not passed
  through.
- **`capabilities` is answered once**, before any snapshot. It drives the merge
  order and lets the core skip a provider that cannot answer the question being
  asked. `cost` is a three-value hint (`low`/`medium`/`high`) so a `high` provider
  (a screenshot reader) is never on the default path.
- **`budget.tokens`** is the core's, not the provider's: a provider that cannot fit
  its answer must summarise or omit, and say which. This is the same discipline as
  `truncated` on a page.
- **No provider is asked twice per turn.** One snapshot per turn, taken before the
  first tool call, so every tool in the turn sees the same world.

## 3. Merge is by field, and precedence is configuration

The registry merges **per field**, not per provider: `cursor` may come from tmux
while `cwd` comes from the hook while `active_tab` comes from Konsole. Precedence is
a list, and it is configuration because it is a statement about the user's setup, not
about the code:

```json
"percept": {
  "providers": [
    {"id": "pty",     "enabled": true},
    {"id": "tmux",    "enabled": true},
    {"id": "hook",    "enabled": true},
    {"command": "pu-percept-konsole", "timeout_ms": 300},
    {"id": "pointer", "enabled": false}
  ],
  "precedence": ["pty", "tmux", "hook", "konsole", "pointer"]
}
```

First provider in `precedence` that reports a field wins it. A field nobody reports
is **absent**, never defaulted (`{"top": 0}` reads as "at the top", which is a
claim). Every field carries the provider id that supplied it, so the envelope can
say `"provenance": "tmux"` and the model can weigh it.

## 4. Wayland + Konsole: the ceiling, stated plainly

Wayland is not a permissions problem that a plugin can route around. A client cannot
query the global pointer, and no emulator exposes its grid. With Konsole under
Wayland, without tmux, the reachable set is:

| Field | Available | Source |
|---|---|---|
| `active_tab`, `title` | yes | Konsole D-Bus `currentSession()` / `title()` |
| `cwd`, `foreground` | yes | `processId`/`foregroundProcessId` → `/proc/<pid>/cwd`, `cmdline`, `stat` field 8 |
| `cursor` | **no** | — |
| `viewport` | **no** | — |
| `mouse` | **no** | Wayland has no global pointer; Konsole never enables reporting on a shell |
| `grid` | **no** | — |
| `last_command`, `exit` | yes, if the hook is installed | shell hook JSONL |

That is still a useful snapshot: *"the user is looking at tab 2, which is running
`vim src/net.cpp` in `/repo/src`, and their last command was `ninja -j8`, which
exited 2."* Four of the five things a person would say if you asked "where am I" —
and none of them requires the screen.

The ceiling moves in exactly two ways, both already in the ladder: the user runs
tmux (`viewport` + `grid` + `cursor`), or pu runs the command itself
(`forkpty`/ConPTY, everything including mouse cells for a TUI).

KWin is not a third way. Its D-Bus scripting exposes windows (title, class, geometry,
active window) and no pointer position; a `kwin` provider would add "Konsole is the
focused window", which the process tree already implies. Do not build on it.

## 5. The generic seam, and what ports

"Generic" here cannot mean one API that reads every terminal — that API does not
exist and the reason is structural (§4). It means three portable things and a
disposable fourth:

**Portable (write once):**
1. **The envelope** — `Snapshot`, field vocabulary, provenance, budget. This is the
   part the model and the tools see, and it must not mention Konsole, tmux or X11.
2. **The pty spawn** — `forkpty` on POSIX, `CreatePseudoConsole` on Windows. Same
   interface: spawn a child on a pty, read a VT stream, report size, kill the group.
   This is the only reader that is identical in capability everywhere, and it is the
   one worth the most.
3. **The plugin protocol** — §2. Any language, any platform, no ABI.

**Disposable (one small file per platform or product):**

| Reader | Linux | Windows | macOS |
|---|---|---|---|
| `pty` | `forkpty` | `CreatePseudoConsole` | `forkpty` |
| `tmux` | tmux | WSL only | tmux (brew) |
| `hook` | bash/zsh/fish → JSONL in `$XDG_RUNTIME_DIR` | PowerShell `prompt` function | zsh/bash |
| `konsole` | D-Bus + `/proc` | — | — |
| `windows-console` | — | `AttachConsole` + `ReadConsoleOutput` (conhost only; Windows Terminal's grid is in the terminal, not readable) | — |
| `iterm` / `terminal-app` | — | — | AppleScript / iTerm Python API |
| `pointer` | X11 only (`xdotool`) | `GetCursorPos` (screen coords) | `NSEvent.mouseLocation` (screen coords) |

Three observations from that table:

- **The pty row is the only one that is genuinely cross-platform**, which is another
  argument for building it first: it is the 60% that ports for free, and it is the
  only source of a mouse cell anywhere.
- **Every `pointer` row gives screen coordinates**, so the same rule holds on all
  three platforms: do not map to a cell. That rule is portable even though the API
  is not.
- **Every emulator row is a walled garden** — Konsole, Windows Terminal and iTerm
  each expose a different subset, and none exposes the grid. That is exactly why
  they belong behind a process boundary: their APIs change at the vendor's pace, and
  a broken plugin should degrade to "that field is absent", not to "pu does not
  build".

## 6. Security and privacy

A plugin executable is arbitrary code execution with the user's privileges — the same
class as an MCP server, and it should be gated the same way:

- **Explicit configuration only.** A provider runs because `agents.json` names it.
  No auto-discovery from `PATH`, no download on first use.
- **Path pinned, cwd sandboxed, timeout enforced** (`timeout_ms`, default 300, kill
  on expiry). Same shape as the MCP timeout and the FFI worker.
- **First run asks** through the existing `ToolContext::Approval` path.
- **Redaction is double-sided.** The plugin reports what it removed; the core applies
  its own pass to the merged snapshot. A plugin bug must not be able to leak a token
  into the transcript, and the transcript is what reaches a third-party model.
- **Snapshots are opt-in per call, not ambient.** A screen or a command line can
  contain secrets; putting either in the system prompt every turn is a data
  exfiltration design, not a feature.

## 7. Shape in this tree

```
include/pu/percept/snapshot.hpp      Field, Snapshot, the closed field vocabulary
include/pu/percept/provider.hpp      Provider, Registry (explicit table, like gates)
src/percept/registry.cpp             the table: id -> factory
src/percept/pty_provider.cpp         POSIX now, ConPTY later
src/percept/tmux_provider.cpp        shells out; cached per turn
src/percept/subprocess_provider.cpp  spawn + JSONL, the plugin host
src/percept/pointer_x11.cpp          opt-in, off by default
```

`Runtime` owns the registry beside `toolbox_` and `executor_`; the snapshot is taken
in `Executor::RunToolLoop` before the first call and handed to tools through
`ToolContext` (the same pointer discipline as `security`). `agents.json` gets the
`percept` block above; `Toolbox` is untouched.

Tests need no display: a fixture plugin executable compiled by CMake that answers
`capabilities` and a canned `snapshot`; assertions on merge order, on an undeclared
field being dropped, on a plugin that times out degrading to absent, and on
`provenance` being stamped by the core rather than trusted.

## 8. Order

| Step | Content | Why |
|---|---|---|
| 1 | `Snapshot` + field vocabulary + registry, with one in-tree provider (`tmux`) | settles the contract before any portability claim |
| 2 | `subprocess_provider` + `pu-percept-konsole` (a script, not C++) | proves the plugin boundary with the least code; Wayland's whole reachable set is here |
| 3 | `pty` provider + `pu run` (POSIX) | the only reader that is complete, and the only one that ports unchanged |
| 4 | hook (JSONL + OSC 133) | `last_command`/`exit` for every terminal, including Windows via a PowerShell `prompt` |
| 5 | ConPTY, then the macOS rows | reuses step 3's interface; no new concept |

## 9. Open questions

1. Should `grid` be a field at all, or is a grid just another `Source` that the
   viewport pages over? The second is cleaner and makes `capture-pane` output page-able
   for free — but it couples the plugin protocol to the viewport's unit model.
2. Is one snapshot per turn right when a turn takes 30 seconds and the user is
   typing? A `stale_after_ms` on the field would let a tool ask for a refresh without
   making every call re-read.
3. Does the hook write a JSONL file or connect to a Unix socket in
   `$XDG_RUNTIME_DIR`? The socket avoids parsing a growing file, but fails on
   Windows; the file is portable and can be tail-read.

## 10. Where attaching to a live Konsole is settled

Step 3 above assumes pu spawns the command. Observing the tab the user already has
open is a different question, and the rungs are ranked in `konsole-attach.md`: a relay
pty is the primary path, a preloaded shim or an eBPF uprobe are the only ways to reach
an existing process, and reading Konsole's own `Screen` is ruled out because a pty
master has one consumer and because the screen is a pure function of the stream.
