# Terminal perception on Linux, with Konsole as the target

`context-viewport.md` §5 says the screen lives in the emulator, not in the tty. This
document is that statement made concrete for Linux, and specifically for the pair
"Arch without a desktop environment" + "KDE Konsole". It answers three questions in
order: what can be read at all, what Konsole itself can and cannot tell you, and
which order to build the readers in.

**Conclusion up front:** yes, adapt for Arch first — but the useful readers are the
pty and tmux, not Konsole. Konsole's own interface is a *control* channel
(`sendText`, `runCommand`, `sendMouseEvent`) plus identity, and it has no screen, no
cursor and no viewport position. Build the readers that also work on a server, and
let Konsole contribute only the one fact nothing else knows: which tab is active.

---

## 1. Two different targets are being called "headless"

Konsole is a Qt application; it needs a display server. So "no desktop environment"
splits into two cases that support different things:

| Case | Display | What exists | Mouse |
|---|---|---|---|
| A. no GUI at all (ssh, `tty`, a container) | none | pty, tmux, shell hooks | not a concept |
| B. X11 or Wayland without a DE (`startx` + konsole, `kwin_wayland` alone) | X11 / Wayland | A, plus window/pointer APIs | only on X11, and only as a **global** coordinate |

Case A is the one worth designing for first, because it is a strict subset of B and
because it is what a build server looks like. Everything below that works in A also
works in B; the reverse is not true.

## 2. What Konsole's interface actually gives you

Verified against the Konsole handbook's scripting chapter, the `org.kde.konsole`
introspection dump, and a 2025 KDE forum thread about exactly this question:

| Method / property | What it does | Useful for reading? |
|---|---|---|
| `Session.sendText(QString)` | inject text into the session's input | no (write path) |
| `Session.sendMouseEvent(buttons, column, line, type)` | inject a mouse event **into a cell** | no (write path) |
| `Session.processId()` / `foregroundProcessId()` | pids of the shell and the foreground job | **yes** — then `/proc/<pid>/cwd`, `cmdline` |
| `Session.title(role)` | the tab title, as set by OSC 0/2 | weak (a title is self-reported) |
| `Session.environment()` | the session's environment block | no |
| `Konsole.currentSession()` | the **active tab** of the instance | **yes** — the one fact only Konsole has |
| anything screen-related | — | **does not exist** |

Two notes that matter for the design:

- **There is no `cwd` in the interface.** The KDE answer is `stat /proc/<pid>/cwd`.
  That is also the answer for the foreground job, which is how you learn "this tab is
  running `vim` in `/repo/src`" without a screen.
- **`sendMouseEvent` is the inverse of what we want.** It proves Konsole thinks in
  `(column, line)`, and that no API exposes the conversion from a pointer position
  to a cell. Injecting is not reading, and it is a privileged action that must never
  be on the perception path.

The one thing worth the D-Bus round trip is `currentSession()`: tab identity is not
derivable from `/proc` or the environment (`$KONSOLE_DBUS_SESSION` names *our* tab,
not the one the user is looking at). Everything else Konsole offers is either a
write path or a `/proc` lookup with more steps.

**Cost:** adding Qt DBus to a Boost-only CLI is not worth it. `busctl --user call`
reaches the same interface, and a missing `busctl` degrades to "identity unavailable"
rather than a build dependency. Note the service name drifts — `org.kde.konsole` for
the first instance, `org.kde.konsole-<pid>` for later ones, which is what
`$KONSOLE_DBUS_SERVICE` is for. Verify the method list with `qdbus6` on the target
before relying on any of it; the interface has changed since the 4.x dumps that most
search results quote.

## 3. "Page position" cannot come from Konsole

Konsole does not expose the scroll offset, the cursor row, or the first visible line.
Neither does any terminal emulator, for the same reason: the viewport is a widget
state, not a protocol state. So the only two honest sources are:

| Source | What "position" means | Fields |
|---|---|---|
| **tmux** (`$TMUX` set) | offset into a history the multiplexer owns | `#{scroll_position}` (0 = bottom), `#{history_size}`, `#{cursor_x}`, `#{cursor_y}`, `#{pane_height}`, `#{pane_width}`, `#{pane_current_path}` |
| **a pty we spawned** | offset into a scrollback we own | our own grid + scroll anchor |

tmux is the better first target on Arch: it is headless-native, needs no
privileges, works under Konsole, `alacritty`, `ssh` and a bare VT alike, and it gives
the visible grid *and* the scrollback with colour intact:

```
tmux display -p -t "$TMUX_PANE" '#{scroll_position} #{history_size} #{cursor_x} #{cursor_y} #{pane_height} #{pane_width} #{pane_current_path}'
tmux capture-pane -p -e -t "$TMUX_PANE" -S -40   # -e keeps SGR, -S reads back
```

`capture-pane` is the closest thing to "what the user is looking at" that exists
without owning the emulator: it is the composed grid, not the byte stream, so
overwrites, `\r` progress bars and alt-screen are already resolved. The visible top
line is `history_size - scroll_position - pane_height + 1` (verify per tmux version;
`scroll_position` counts from the bottom). A user scrolled up 200 lines is a **strong
intent signal** — it means "I am reading history, not the tail" — and it is the
closest analogue to your "page centre".

## 4. Mouse: the coordinate you can get is the wrong one

On X11, `xdotool getmouselocation` gives a root coordinate. Turning it into a Konsole
cell needs the window geometry, the frame and tab-bar insets, the profile's font
metrics, the scrollbar width and the DPI — six inputs, all of which change with
theme, zoom and multi-monitor. Konsole will not do the conversion for you. A
perception feature that silently reports the wrong cell is worse than one that
reports nothing.

On Wayland, a client cannot query the global pointer at all; that is a deliberate
security property, not a missing feature.

The mouse position that *is* exact comes from a different route: a program that
enables mouse reporting (DECSET 1000/1002/1003, SGR 1006) sends `(col,row)` **as
data** on the pty. If we own the pty, we read it; no mapping, no font metrics. And
a normal shell prompt never enables it, so for a shell there is nothing to read —
which is the correct answer, not a gap to work around.

Therefore:

- **Do not** build global-coordinate → cell mapping. It is the least reliable part of
  the whole feature for the least information.
- **Do** read mouse reports from a pty we own, for TUIs (`vim`, `htop`, `less` with
  mouse). That is the only place a pointer is meaningful, and it arrives free.
- **Do** treat "scrolled up" (tmux `scroll_position > 0`) as the headless substitute
  for "where the eye is".

## 5. The reader ladder, with per-field provenance

Precedence is by **field**, not by provider: a cursor may come from tmux while the
cwd comes from the shell hook while tab identity comes from Konsole. One snapshot,
merged, each field labelled.

| Reader | cursor | viewport offset | mouse cell | SGR | cwd / last command | needs |
|---|---|---|---|---|---|---|
| `pty` (we spawned) | own VT | own scrollback | yes, if child enables | yes | from `OSC 7`/cwd of child | `forkpty` |
| `tmux` | yes | **yes** | no (tmux consumes it) | yes (`-e`) | `pane_current_path`; command via hook | tmux |
| `shell hook` | no | no | no | no | **yes** (cwd, cmd, exit, times) | a `precmd`/`preexec` hook writing JSONL |
| `konsole-identity` | no | no | no | no | tab title; pid → `/proc` | `$KONSOLE_DBUS_SESSION` (+ `busctl` for the active tab) |
| `x11-pointer` | no | no | global only | no | no | X11; **opt-in, off by default** |
| `wayland-pointer` | — | — | impossible | — | — | — |

```json
{
  "cursor":   {"row": 12, "col": 40, "provenance": "tmux"},
  "viewport": {"top": 3480, "height": 40, "scrolled_up": 0, "provenance": "tmux"},
  "cwd":      {"path": "/repo/src", "provenance": "hook"},
  "active":   {"tab": "konsole:/Sessions/2", "foreground": "vim", "provenance": "konsole+proc"},
  "mouse":    {"present": false, "why": "foreground app has not enabled reporting"}
}
```

Rules that keep this from becoming a lie:

1. **A field with no reader is absent, not defaulted.** `{"top": 0}` when nothing
   knows the offset reads as "at the top", which is a claim.
2. **`scrolled_up` changes the default anchor.** If the user is 200 lines up, the
   window should open there, not at the tail — this is the one case where the
   snapshot should override `view=tail`.
3. **Pty readers are per-child, not per-session.** Owning the pty of a command we ran
   says nothing about the user's tab; do not merge the two under one `provenance`.
4. **`konsole+proc` must degrade to `proc`.** If `busctl` is missing or the service
   name drifted, "which tab is active" is unknown while "which pids exist" is not.

## 6. What to build on Arch, in order

### Step 1 — pty capture (`src/exec/run.cpp`)

The POSIX path already does `fork` + `pipe` + `setpgid` + `kill(-pid)`; `forkpty`
replaces the pipe with a pty and the rest stays. Three decisions that are easy to get
wrong:

- **Not the default for `execute_bash`.** A pty changes the child's behaviour:
  colour on, line-buffering off, `isatty()` true, progress bars, and — the dangerous
  one — a program that reads stdin now has a tty to read from and can block. Make it
  `tty: true` per call, or an explicit `pu run -- <cmd>`, and keep the pipe path as
  the default.
- **Set the size** (`TIOCSWINSZ`) before the child draws, or everything wraps at 80.
- **Strip what the terminal adds**: `\r` before `\n`, and the echo of anything typed.
  A pty is a device with echo on by default; a capture is not a terminal session.

A zero-code sanity check on Arch before writing any of it:
`script -qec 'ls --color=always' /dev/null | cat -v` — if SGR survives that, the pty
path will too.

### Step 2 — tmux provider

Pure shell, no C++ until the shape is settled: one `tmux display -p` for the fields,
one `capture-pane -e` for the grid, both cached for a turn (a snapshot older than the
tool call it informs is a guess). Guard on `$TMUX`; treat a missing tmux as
"unavailable", not an error.

### Step 3 — shell hook

A `precmd`/`preexec` pair appending JSONL to `$XDG_STATE_HOME/pu/prompt.jsonl`
(cwd, command, exit, timestamp, tty, `$TMUX_PANE`). Emit OSC 133 as well, so
Konsole's own semantic features work — cooperation, not interception. This is the
only reader that needs the user to install something, and it is also the only one
that answers "what did they just run", which is most of what "where is the user"
means.

### Step 4 — Konsole identity (optional)

`$KONSOLE_DBUS_SESSION` / `$KONSOLE_DBUS_SERVICE` for our own tab; `busctl --user` for
`currentSession()`; `/proc/<pid>/cwd` and `/proc/<pid>/stat` field 8 (`tpgid`) for the
foreground job. No Qt, no plugin, no KPart.

### Deliberately not built

- **OCR of an X11 screenshot.** Lossy text, a recording permission, and a privacy
  hazard, to recover something tmux gives exactly.
- **Global pointer → cell mapping.** §4.
- **A Konsole plugin or KonsolePart front end.** It would own the emulation and give
  a perfect grid — for terminals it created, which the user is not using. The same
  effort buys `pu run` for every terminal.
- **`TIOCSTI` injection or `/dev/tty` reads.** Disabled by default since Linux 5.9
  and destructive to the user's input respectively.

## 7. Testing without a display

Everything above except step 4 runs headless, which is the point:

- pty: a test that runs `printf '\033[31mred\033[0m\r\n'` through the new path and
  asserts the SGR is parsed and the `\r` is gone.
- tmux: `tmux -L putest new-session -d -x 100 -y 30`, write known lines, assert
  `scroll_position`/`cursor_y` and a `capture-pane` slice. `tmux -L` gives a private
  socket, so CI never touches a real server.
- hook: pure JSONL parsing, no shell needed.
- Konsole: assert the *degradation* — with `KONSOLE_DBUS_SESSION` unset, the snapshot
  has no `active` field and does not fail.

## 8. Open questions

1. When tmux and the pty disagree (a command pu spawned inside a tmux pane), which
   wins for "viewport"? Probably the pty for the command's own output and tmux for
   the pane, but the two must not be merged into one `top`.
2. Is `scroll_position > 0` enough to override the anchor automatically, or should
   it only be offered as a hint in the envelope? It is the one inference that can
   silently mislead.
3. Does a snapshot belong in the system prompt (always fresh, costs tokens every
   turn) or behind a tool call (explicit, but the model must know to ask)? A
   one-line digest in the prompt plus a full snapshot on demand is probably the
   split, and it needs a measurement to settle.

## 9. See also

[percept-plugins.md](percept-plugins.md) — how these readers become plugins without
a shared library, the field vocabulary they report into, and which rows of the table
above port to Windows and macOS unchanged.
