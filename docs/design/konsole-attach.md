# Attaching to a Konsole session: what a hook can and cannot buy

The wish is to observe the terminal the user is already sitting in — the tab, the
cursor cell, the scroll position, the pointer — rather than only the commands pu ran
itself. The plausible mechanisms are a CLI that owns a pty, an MCP server that answers
questions about the screen, and a hook placed inside Konsole by preloading or tracing.

This note sorts those into five rungs, says which of them can work at all, and
records why the reversing rung is worth less than it looks.

## 1. Three facts that decide the rest

1. **The screen is not in the kernel.** A pty carries the byte stream, the line
   discipline and a small edit buffer. The grid, the scrollback and the cursor cell
   live in Konsole's own memory, in the classes that consume that stream. There is no
   API to ask an emulator what it is displaying, on any platform.
2. **A pty master has exactly one consumer.** Bytes written to the master are handed
   to whichever process reads the slave first; bytes written to the slave are handed
   to whichever process reads the master first. Two readers *split* the stream, they
   do not both see it. So "attach to a live session without disturbing it" is not a
   matter of finding the right fd — duplicating the fd makes Konsole's own screen
   lose the bytes the observer consumed.
3. **The screen is a pure function of the stream.** Replaying the VT stream through
   our own parser produces the same grid, cursor and attributes Konsole shows,
   deterministically, for the cost of one pass over bytes we already have. Anything
   that reads Konsole's internal state directly is buying back a computation we can
   do correctly, with a version-locked struct layout as the price.

Fact 2 is what makes this decision narrow. Fact 3 is what makes reversing unattractive.

## 2. The ladder

| Rung | Mechanism | Yields | Precondition | Failure mode |
|---|---|---|---|---|
| L0 | shell hook (`precmd`/`preexec` → JSONL, plus OSC 133) | `last_command`, `exit`, `cwd`, duration | one line in `.bashrc` | a shell that does not source it yields nothing |
| L1 | D-Bus `org.kde.konsole` + `/proc` | `active_tab`, `title`, `foreground`, `cwd` | none | an API rename — degrade to absent field |
| L2 | **relay pty**: Konsole's profile runs `pu shell`, which opens an inner pty | everything: grid, cursor, attributes, `viewport`, mouse cells, resize | the tab is started through pu | pu crashes and the tab dies with it |
| L3 | `LD_PRELOAD` shim inside Konsole, teeing the master fd | as L2, for tabs pu did not start | Konsole launched with the env var — i.e. a restart | missed syscall form (e.g. `readv`) → silent blind spot |
| L4 | trace the `read`/`write` syscalls with an eBPF uprobe, copying the buffer | as L2, on a running Konsole, no injection | root or `CAP_BPF`/`CAP_PERFMON` | kernel/ptrace policy; not shippable as a default |
| L5 | `ptrace`/`process_vm_readv` into `Emulation`/`Screen`, or `pidfd_getfd` | the buffer without re-parsing | `ptrace_scope=0` or a privileged helper; debug info; per-version offsets | wrong offset reads garbage silently; the fd dup steals the stream (fact 2) |

L5 is the rung people mean by "reverse it". Both of its halves fail for reasons that
have nothing to do with effort:

- `pidfd_getfd` duplicates the master fd, which by fact 2 turns the observer into a
  competing reader — Konsole's screen would start dropping output. It is a
  *takeover* primitive, not an observation primitive.
- Reading `Screen` means knowing where its `Character` spans and its history ring
  sit. Arch splits debug info into `konsole-debug`, so the offsets are obtainable,
  and they change with the next Konsole release. A parser we own does not.

So the honest ranking is: **L2 for the general case, L4 when a live tab must be
observed without a restart, L0+L1 always on, L5 not at all.** L3 sits between L2 and
L4: it needs a restart like L2 changes the launch, but it keeps the user's own
Konsole process, which matters if their profile, fonts and scrollback are the point.

## 3. Why the relay is the primary path

`pu shell` becomes Konsole's child instead of Konsole's observer:

```
Konsole ── outer pty (master) ──> pu shell ── inner pty (master) ──> $SHELL
                <── stdin ──            <── passthrough ──
```

- **Bytes are forwarded verbatim in both directions**, so colours, `TERM`, alt-screen
  apps, mouse reporting, bracketed paste and Konsole's own scrollback all behave
  exactly as before. Nothing is re-rendered; the parser only *watches*.
- **`isatty()` stays true** for the shell, so interactive programs keep their
  behaviour. This is the opposite of the earlier rule that `execute_bash` must not use
  a pty by default: there the caller wants captured text, here the user wants a
  terminal.
- **Resize is two calls**: `TIOCGWINSZ` on our outer stdin, `TIOCSWINSZ` on the inner
  master, then `killpg(SIGWINCH)`. Without this the shell stays at 80 columns.
- **The relay must stay dumb.** It is a `poll`/`read`/`write` loop over two fds plus a
  tee into a pipe; the VT parser and the snapshot builder live in a *child* process.
  A bug in the parser then costs a snapshot, not the user's session. A `--no-observe`
  flag should reduce `pu shell` to `exec $SHELL` so the feature can be ruled out on a
  machine where it misbehaves.
- **Input is not ours to keep.** The direction Konsole→inner carries `sudo` passwords,
  so the relay forwards those bytes and drops them; only the output direction is
  parsed. Additionally, `tcgetattr` on the master reports the line discipline, so
  `!(c_lflag & ECHO)` marks a moment when the user is typing something the terminal
  itself is hiding — capture should be suspended there rather than trusting a
  pattern list.

Cost: one extra process per tab, one extra hop of copy, and the inner pty must be
created with the outer window size already set, or the first prompt wraps wrongly.

## 4. What L3 needs confirmed before it is written

The shim's whole job is to see the master fd's traffic, which means intercepting the
calls Konsole actually makes. Verify rather than assume:

```bash
strace -f -e trace=read,readv,write,writev -p "$(pidof konsole)" 2>&1 | grep -m5 'readv\|read('
```

If the reads arrive as `read()` on an fd that `ioctl(TIOCGPTN)` accepts, a `read`
wrapper is enough. If they arrive as `readv()` or through a Qt-internal buffer, the
wrapper has to cover that form too, and the "silent blind spot" failure mode above is
the thing to test for, not the happy path. Konsole also has to be *started* with
`LD_PRELOAD`, which means editing the `.desktop` file or the profile's `Command=` —
the same edit L2 needs, minus the inner pty. That is why L3 is a fallback, not a
first move.

## 5. Two front-ends, one daemon

The relay is the only writer of the stream; everything else is a reader of snapshots.
So the shape is a socket with one producer and N consumers:

```
$XDG_RUNTIME_DIR/pu/percept.sock     # the relay listens; JSONL, request/response
                                     # plus {"event": ...} frames for changes
pu percept show                      # human/debug dump of the latest snapshot
pu percept mcp                       # MCP server over stdio, a shim over the socket
```

`pu percept mcp` is where the MCP request from the brief lands, and it adds no
perception logic: the project already has JSON-RPC framing and a stdio transport, just
on the client side, so the server is the same framing with a handler table. The tool
surface stays at three, for the reason the capability roadmap gives — every schema is
sent every turn:

| Tool | Answers | Notes |
|---|---|---|
| `terminal.snapshot` | tab, `cwd`, `foreground`, `last_command`/`exit`, grid size, cursor, `viewport` | fields absent when unobserved, never invented |
| `terminal.read` | a window of the grid, reusing the pager's envelope | same `Viewport`/cursor discipline as `execute_bash` |
| `terminal.wait` | blocks until the focus, the foreground command or the exit status changes | a model cannot poll cheaply; this replaces polling |

`terminal.wait` is the part that matters and the part a passive reader cannot do. A
snapshot on demand answers "where is the user", but the interesting question is "tell
me when the build finishes", and only the stream can answer it. It is also the
argument for the relay over any observation-only rung: L4 and L5 can read, but the
change notification has to come from whoever holds the stream.

## 6. Privacy, stated once

A terminal snapshot is the most sensitive artifact this project would ever hold: the
screen routinely contains tokens, keys and customer data, and the input direction
routinely contains passwords. Therefore:

- capture is **opt-in per invocation** — the model asks, the user sees the ask, the
  snapshot is not a background default;
- output-only parsing, `ECHO`-off suspension, and a redaction pass over the merged
  snapshot, in that order of trust;
- sessions the user did not start through `pu shell` stay unobserved unless they
  explicitly attach. This is a policy choice, not a limitation — the alternative is a
  tool that silently watches every tab.

## 7. Order

1. `core/process.hpp` with `forkpty` + `TIOCSWINSZ` + `killpg`, and `pu shell` as a
   byte relay with `--no-observe`. No parser yet; verify that a real Konsole tab
   behaves identically, including alt-screen, resize and `htop`.
2. A VT parser behind the relay, producing `grid`/`cursor`/`attributes`, plus the
   socket protocol and `pu percept show`. Settle the field vocabulary here.
3. `terminal.read` and `terminal.wait` on top of the pager envelope.
4. `pu percept mcp`, a shim over the socket.
5. L0 hook (JSONL + OSC 133) and L1 D-Bus identity — cheap, and they cover tabs pu did
   not start.
6. L4 uprobe behind an explicit `pu percept attach` that states it needs privilege;
   L3 only if users ask to retrofit existing Konsole windows; L5 never.

## 8. Open questions

- Does `pu shell` belong in `pu` at all, or is it a separate tiny binary? A relay that
  can be replaced without rebuilding the CLI is easier to trust, and its crash takes
  the tab with it.
- Should the socket carry a full grid, or only the viewport plus a line index, with
  `terminal.read` paging into a `HistoryScroll`-like structure? The second matches the
  pager budget rules and keeps a 100k-line scrollback out of memory.
- For `terminal.wait`, is "foreground command changed" the right trigger, or does the
  model need "output stopped arriving for N ms" as well? The second is what a person
  means by "it finished".
