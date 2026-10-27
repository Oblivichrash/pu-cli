# A viewport over any source, and where the information about it comes from

`bash-output-window.md` records the first instance: a command's output, paged by
line. This document is the general shape that instance was an instance *of*, and
the second question behind it — if a terminal is designed for a person, what does
the harness owe a model that is reading one?

**Conclusion up front:** the thing worth abstracting is not the pager. It is the
triple *(source, anchor, budget)* and the sidecar that says what is in the window
and why. Retention, selection and presentation are three concerns that the current
`paging` fuses into one line-indexed class; only the first is text-specific.

---

## 1. What is bash-specific today, and what is not

| Piece | Text-specific? | Note |
|---|---|---|
| `CaptureStats`, byte budget, `truncated` | no | any producer with a stream |
| `line_starts_`, 1-based line numbers | yes | unit is line |
| `head`/`tail` first page | no | both are "first" and "last" under a unit |
| `Cursor` `<buffer>:<line>` | half | the *anchor* generalises, the unit does not |
| `Pager` LRU by bytes | no | any retained source |
| `window` JSON | no | field names are line-flavoured |

Four sources already queued behind this abstraction, none of them a shell command:

1. a file read by a tool (`read_file` currently has `offset`/`limit` and no cursor);
2. a directory listing or a `grep -r` result — rows, not lines;
3. a diff — hunks are the natural unit, and a hunk boundary is not a line range;
4. a terminal screen — a *grid*, where a line is a row and columns carry meaning.

Add a fifth that is not text at all: a decoded image or a parsed AST returned by a
native call (§7 of `native-library-calls.md`). A tile is a window position too.

## 2. The abstraction

```cpp
namespace pu::view {

// How a source counts itself. A window is always a span of units, and the unit is
// what makes "the middle of the page" meaningful.
enum class Unit { kLine, kRow, kByte, kHunk, kTile, kCell };

struct Extent { std::size_t total; bool complete; };  // complete=false: unknown/truncated

class Source {                     // what can be looked at
 public:
  virtual std::string Id() const = 0;
  virtual Unit unit() const = 0;
  virtual Extent Measure() const = 0;
  virtual std::string_view Slice(std::size_t first, std::size_t count) const = 0;
  virtual ~Source() = default;
};

// Where the reader is looking. An anchor is opaque to the viewport: a line, a byte,
// a row/column pair, a hunk index.
struct Anchor { Unit unit; std::size_t at; std::size_t column = 0; Provenance how; };

struct Window {
  std::string source;
  Unit unit;
  std::size_t first, last;        // 1-based, inclusive; 0/0 when empty
  Extent extent;                  // of the source, not of the window
  Anchor focus;                   // what the window is centred on, if anything
  std::vector<Mark> marks;        // §3, sidecar
  std::optional<std::string> earlier, later;
  Provenance provenance;          // captured | emulated | reported | inferred
};

Window Open(const Source&, std::optional<Anchor> anchor, Budget budget, Mode mode);

}  // namespace pu::view
```

`Mode::kBrowse` returns a contiguous page (today's pager). `Mode::kFocus` centres on
the anchor and pads out to the budget — the editor's "reveal", not the pager's
"page 1". Both are the same arithmetic with a different start.

**Keep the cursor, demote it.** `<source>:<anchor>` survives unchanged; only the
meaning of the second half becomes unit-relative. A cursor that names a source the
session no longer holds is still refused, not resolved to a neighbour (§3 of the
bash document).

**Do not template this.** `Source` is type-erased on purpose: `Slice` returns bytes,
and a domain adapter decodes. A `Viewport<T>` would need the model to know `T` to
read the envelope, and the JSON that reaches the model must not depend on a C++
template argument. The adapters are where domain knowledge lives (a hunk selector, a
grid selector), and each is small.

**Where over-unification bites.** An image window is not a span of bytes a model can
read; it is a crop plus a scale, and the model consumes it as an image, not as text.
Leave `kTile` in the enum so the envelope is uniform, but let the payload be a media
reference rather than text. Forcing pixels through `Slice` would be the abstraction
eating its own tail.

## 3. Information the terminal has that a text slice does not

A person reading a terminal is not reading bytes in order. They are reading a
**salience field**: a few things stand out and the rest is skimmable. The cues are
not decoration; each is a summary the reader would otherwise have to compute.

| Cue | What it tells a reader | Cheap to capture? |
|---|---|---|
| recency (bottom of scrollback) | this is the part that just happened | yes, free |
| proximity to the prompt / cursor | this is where work stopped | yes, if we own the pty |
| colour + weight (SGR) | error / warning / added / removed / matched | yes, but only in a pty |
| structure (`path:line:`, diff `@@`, `#` headers) | where a section begins | yes, by rule |
| alignment (columns) | which fields are separate | only in a grid |
| motion (progress, spinner) | this is not the final state | needs the running stream |
| rarity (one line differs from 10 000) | this is the anomaly | computable, not free |

Three rules follow, and they are the whole design:

1. **Compute attention at the source, do not ask the model to infer it from text.**
   The harness knows the cursor was at row 12, that the line was red, that 3 of 900
   lines matched. The model would have to rebuild all three from words, twice
   (once per page). Pass them as data.
2. **Marks are a sidecar, never inline markup.** `\x1b[31m` is 5 bytes and about two
   tokens *per styled run*; a 100-line coloured diff pays ~300 tokens to say what a
   range array says in 20. Inline tags (`<error>`) are worse: the model must parse
   them and they collide with the file's own text. Marks are `[[first,last,kind]]`.
3. **No line is hidden silently.** `marks` ranks; it never filters. A window always
   reports `extent` and the ranges it did not show. A model that disagrees with the
   ranking must be able to say "show me 400–600" and get it.

### What the marks mean is per-tool knowledge, not a global truth

`git diff` uses red for *removed*, and an error is also red. `ls` colour depends on
`LS_COLORS`, which is local state. So the capture layer records the fact —
`fg=red, bold` — and the *rule table* (a config file, not code) maps facts to
`kind`. Keep the fact and the interpretation separate; a wrong rule is then a config
change, and a new tool is a rule, not a build.

Two more decisions:

- **De-wrap before paging.** An 80-column terminal breaks one logical line into
  three display rows. Wrapping is a rendering artefact and it corrupts the unit: a
  page boundary can fall inside one line of a log, and "line 41" then means a
  screen row. For line-oriented output, join continuation rows and page by logical
  line. Only for *screen* programs (an editor, `htop`, `tmux`) is the grid the
  content; those get `kCell` and no de-wrapping. The unit is the thing that decides.
- **Do not colourise text back into the transcript.** If the model needs
  "which lines changed", a mark range is enough. Re-emitting ANSI spends tokens to
  reproduce a channel the model does not have.

## 4. Intent is an argument, and it can be a pointer

The observation behind this section: a person says "this one" and points. The
pointing gesture — cursor, selection, the file they have open, the cell under the
mouse — *is* the query, and its information content is higher than any words they
would use to describe it. A model with no pointer has to describe the target in
text and hope the description lands on the same bytes.

So `anchor` takes either a value or the pointer:

```json
{"anchor": "auto"}                        // the last human focus, provenance=inferred
{"anchor": {"line": 412}}                 // explicit
{"anchor": {"path": "src/net.cpp", "offset": 31}}  // a file the user has open
{"anchor": "end"}                         // what a prompt means after a command
```

`auto` resolves against the perception snapshot (§6): the user's selection if there
is one, else the cursor, else the last output. It must be labelled `inferred`, and
the envelope must say what it resolved to. An inferred anchor the model cannot see
is the second opinion problem again, in a new place.

Three consequences worth stating, because they change the default:

- **After a build fails, the answer is not the tail.** It is the first error, which
  may be 3000 lines up. `anchor: "first-mark-of-kind:error"` is one call;
  `view=tail` followed by 20 pages up is the current cost. This is the strongest
  argument for marks over `head`/`tail`: the human behaviour being copied is
  *jumping*, not *paging*.
- **A file about to be edited is read around the edit site**, not from byte 0.
  `read_file` with an anchor is more useful than `read_file` with `offset`, because
  the caller does not have to know the file's shape to name the place.
- **`find` beats `next`.** Add a server-side search over a retained source that
  returns `[line, snippet]` pairs. One call replaces N pages and the ranges are
  exact. A pager without search is a pager a person would refuse to use.

## 5. The terminal is a source, and the honest way to get it

The question was: can we attach to the terminal and see where the user's cursor and
mouse are? The answer has a hard part and a soft part.

**Hard part: the screen is not in the kernel.** A tty keeps a line discipline and a
small edit buffer; what is *on screen* lives in the terminal emulator's memory
(GNOME Terminal, iTerm2, Windows Terminal). There is no portable API to ask an
emulator what it is displaying, and none to ask where its mouse is. `ptrace` on the
shell therefore gets you the shell's memory, not the screen. This is not a
permissions problem to be unlocked later; it is where the pixels live.

| Route | What you get | Verdict |
|---|---|---|
| Own the pty (`forkpty`, `CreatePseudoConsole`) | the whole VT stream: grid, cursor, SGR, alt-screen, mouse reports | **the only reliable route** |
| `TIOCGWINSZ` on a tty | rows × columns, nothing else | useful, trivial |
| `AttachConsole` + `ReadConsoleOutput` (Windows) | the screen buffer, **only** for conhost-owned consoles | fails under Windows Terminal, which is ConPTY |
| `GetCursorPos` (Windows) / `CGEvent` (macOS) | the desktop mouse position | not a terminal coordinate; mapping needs window rect + font metrics, and breaks on DPI |
| `ptrace` / eBPF / `TIOCSTI` | keystrokes at best; `TIOCSTI` is disabled by default since Linux 5.9 | privileged, fragile, still no screen |
| Screen capture + OCR (macOS/Windows) | lossy text, needs a recording permission | last resort, not a source of truth |

**Mouse position in a normal shell does not exist as a signal.** Terminals report
mouse coordinates only when the foreground *application* asks for them (DECSET
1000/1002/1003, SGR 1006), and the reports arrive on the pty as escape sequences.
A shell prompt never enables that, so there is nothing to observe — even for the
pty owner. Mouse-as-intent is available for TUIs and for a GUI front end of our own,
not for `bash`.

**Soft part: shell integration is the cheap 80%.** OSC 133 (prompt start, command
start, command end with exit code), OSC 7 (cwd as a `file://` URI) and OSC 9;9
(Windows Terminal cwd) are emitted by a shell hook and consumed by the emulator.
They give *what the user just ran, where, and whether it worked* — which is most of
what "where is the user" means in practice — without owning anything. They do not
give the screen. Ship both: integration for the common case, pty ownership when the
command runs under us.

**Grid versus stream.** Once a pty is owned, the capture has two views of the same
bytes: the stream (what was printed) and the screen (what is visible now, after
cursor moves, overwrites and alt-screen). They are not interchangeable; a progress
bar printed with `\r` is one line in the stream and 40 states in the screen. Keep
the grid for `kCell` sources and the stream for line sources, and never silently
substitute one for the other.

**Provenance is mandatory.** Every perceived field says how it was obtained:
`emulated` (we ran the VT), `osc7` (the shell told us), `inferred` (a heuristic),
`unavailable`. A model told "cursor at (12,40), provenance: inferred" can hedge; a
model told a bare coordinate cannot. This is the same discipline as `truncated`:
state what the channel does not carry.

**Privacy is the boundary condition.** A screen snapshot is the most sensitive
artefact this tool can produce — tokens, keys, and private mail are routinely on
screen. It must be an explicit call with a visible notice, never ambient capture,
never part of a system prompt, and it must pass a redaction pass first. Recording
it in the transcript means it is sent to a third-party model; that has to be the
user's decision at the moment of the call, not a default.

For the Linux instantiation of this section — which readers exist on Arch, what
Konsole's D-Bus can and cannot answer, and why the mouse coordinate you can read is
the wrong one — see [terminal-perception-linux.md](terminal-perception-linux.md).

## 6. Perception snapshot (the shape, not the claim)

```json
{
  "tty":    {"cols": 120, "rows": 40, "alternate": false, "provenance": "emulated"},
  "cursor": {"row": 12, "col": 40, "provenance": "emulated"},
  "focus":  {"kind": "selection", "from": [12, 8], "to": [12, 60],
             "provenance": "emulated"},
  "cwd":    {"path": "/repo/src", "provenance": "osc7"},
  "last":   {"command": "make -j8", "exit": 2, "ended_ms_ago": 1800,
             "provenance": "osc133"},
  "mouse":  {"present": false, "why": "reporting disabled by the foreground app"},
  "redacted": ["aws_secret_access_key"]
}
```

Every field is optional and every field is labelled. The snapshot is a *source*
(§2) as well as a set of anchors: `Window{source: "screen", anchor: cursor}` is the
"read me what is under my cursor" operation, and it needs no new tool.

## 7. Config and budget

The `output` block added for `execute_bash` becomes `viewport` (keep `output` as an
accepted alias for one release):

```json
"viewport": {
  "page_units": 120,
  "max_units": 400,
  "max_bytes": 262144,
  "marks": true,
  "mark_budget": 0.1
}
```

`mark_budget` bounds the sidecar as a fraction of the page, because metadata is
context too and an unbounded mark list is a new way to overflow. At 0.1, a 400-line
page may spend 40 lines' worth on marks; beyond that the lowest-scoring marks are
dropped and `marks_omitted` is reported.

## 8. Migration

1. **Mechanical, no behaviour change.** `paging/paging.*` → `viewport.*`; `line`
   becomes a `Unit` whose only implemented value is `kLine`; `first_line`/`last_line`
   stay in the JSON as aliases of `first`/`last`.
2. **`read_file` takes an anchor and a cursor.** It already has `offset`/`limit`;
   this gives it the same envelope and the same pager, so a file read is resumable.
3. **`find` over a retained source.** Search, jump, then page. The cursor stays.
4. **Marks.** SGR capture only matters once §5 exists; until then, structure rules
   (`path:line:`, `error`, diff prefixes) are enough to prove the format.
5. **pty source** behind a setting, `pu run -- <cmd>`; ConPTY on Windows.

Steps 1–3 are worth doing first and cost little: they turn one tool's pager into a
session capability, and they make `read_file` obey the same budget as `execute_bash`
— which today it does not.

## 9. Routes not taken

- **Wrap everything in a generic `Resource`/`Provider` interface** with its own
  registry and lifecycle. Two sources and four units do not justify a plugin system;
  the registry would be more code than the sources.
- **Send ANSI to the model and let it interpret colour.** It costs tokens per styled
  run, it depends on the producer deciding to emit colour at all (most do not when
  stdout is a pipe), and "red" has no stable meaning across tools.
- **Infer the anchor from the command string inside the tool.** Recorded already in
  the bash document, and it stays true here: a heuristic the model cannot see is a
  second opinion. `auto` is allowed *because* it is visible in the envelope and
  labelled `inferred`.
- **Capture the screen by scraping another process's pty.** Requires ptrace, gives
  keystrokes rather than the screen, and breaks on every kernel bump.
- **Make the grid the universal unit.** Logs are lines; forcing them through a cell
  grid preserves wrapping artefacts and multiplies the payload by the column count.

## 10. Open questions

1. Should `marks` be computed at capture time (in the tool) or at selection time
   (in the viewport)? Capture-time is cheaper per page and survives re-paging;
   selection-time can use the anchor's kind as a filter.
2. Is `kHunk` worth a unit, or is a diff just lines plus a mark per hunk header?
   The answer decides whether `earlier`/`later` for a diff mean "hunk" or "page".
3. For an inferred anchor, is `provenance: inferred` enough, or does the envelope
   need to carry *all* candidates it chose between, so the model can overrule?
4. Screen snapshots are sensitive enough that they may belong behind the approval
   gate (`ToolContext::Approval`) rather than behind a config flag.
