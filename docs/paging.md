# Paged tool output

A command can print more than a turn can carry. `execute_bash` therefore answers with one
page of what the command printed, and keeps the rest where a later call reads it without
running the command again.

## What one call returns

Beside the usual `success`/`output`/`error`/`exit_code`, a result carries three things a
model reads and one it does not:

```json
"lines":  {"first": 3401, "last": 3520, "of": 3520},
"stored": {"printed": 3520, "kept": 240, "dropped": 3280,
           "printed_bytes": 148224, "kept_bytes": 9871, "kept_from": "the end"},
"read_more": {"how": "call execute_bash again with at=<line number>, or with query=<text>...",
              "covered": {"first": 0, "last": 240}},
"buffer": {"id": "o3f2a1c4-1", "source": "s3f2a1c4", "rank": 7}
```

- **`lines`** is where this page sits. `first`/`last` number the lines of the command's
  whole output, so a page says where it is even when the buffer holds one end of it, and
  `of` is how many lines the command printed.
- **`stored`** is how much survived. `printed` is what the command printed, `kept` what
  the buffer kept, `dropped` the difference, and `kept_from` whether the kept lines are the
  start or the end of the output. A lone `truncated: true` said that something was missing
  without saying whether it was worth going back for; three numbers say whether the text a
  model wants is gone rather than merely further down.
- **`printed_bytes`/`kept_bytes`** are the same account in bytes. Both units are reported
  because they answer different questions: a model reasons in lines — it follows a hit up
  by line number and reads a log from its last line — but a line is not a fixed size, and
  "how much context did this cost" is a question only bytes answer. A 200-line minified
  JSON dump and a 200-line source file are the same `printed` and nothing alike in `bytes`.
  `kept_bytes` is the size of the buffer, not of the page just shown.
- **`read_more`** is absent when the whole output is in hand. When it is present it names
  the two ways to see more, and the range of lines they can reach.
- **`buffer`** is for the machine, not the model. `id` names the run, `source` names the
  question — the command and the directory — so two runs of one command share a source, and
  `rank` says how late the run was, which is what makes one page supersede another. It is
  present because `OriginOf` and `TombstonedReceipt` read it to know whether a newer run
  replaced this one; a model is never told to read it, and the tool description no longer
  names it.

`at` says where to read: omitting it runs the command and starts at its last line, `head`
runs it and starts at the first, and a line number reads the stored output without running
anything. `query` searches that stored output. `limit` asks for a number of lines, clamped
to `max_lines`.

## A page is sized like a terminal window

The page sizes are chosen to match what a person sees reading a command in a terminal, not
to a token count: `page_lines = 80`, about two screens of scrollback, which is what a
developer asks for when they say "show me the end of the log".

A line count alone is not a size, though. A minified JSON document is one line of two
megabytes, and a page of one such line is the whole context. So a page is bounded twice:
by `page_lines` and by `page_bytes` (16 KiB, roughly four times the text of 80 ordinary log
lines). The line bound decides an ordinary page of logs, and the byte bound decides a page
of packed machine output. A single line wider than the byte bound is still shown — one line
is the smallest page there is, and an empty page cannot be told from a command that printed
nothing.

## Reading stored output needs no command

`at: "3281"` reads from that line. The line number names the newest run, because that is
the only run a model that just saw a truncated result can mean, and restating a
200-character command to read page two would be a second source of truth that can only
disagree with the first.

`query: "error"` returns every line of the newest run containing that text, with `context`
lines either side, each prefixed by its own line number so a hit can be followed up with
`at`. It is a substring search, not a regular expression: a model that wants a pattern can
run `grep` itself and pay for the output it asks for, and a search that has to agree on a
dialect would be a second language inside a tool that already has one. A miss names how
much was dropped, so a search of a truncated run does not read as an absence of the text
from the command.

Two limits follow from the store being a store:

- **Reading never re-runs the command and never replaces what is stored.** Only running a
  new command stores a new run, so a model may read as much of one command's output as it
  needs before running the next.
- **A stored run expires only when it is dropped** — the last 8 runs within 4 MiB, both
  configurable under [`paging`](configuration.md#paging). The buffers belong to the
  runtime, not to a tool, so a page still reads after the toolbox is rebuilt, which is what
  switching model or agent does. A buffer id carries the mark of the pager that stored it,
  so a cursor written by an earlier process finds nothing here rather than a namesake — a
  bare counter would have made the first run of every process the same run. A line number
  whose run is gone is refused.

## A turn keeps one page per source

Every tool result is stored in the conversation and sent again on every later turn. A page
left as it was would therefore cost its lines again and again, and a model that pages ten
times would fill a context with nine pages it has already read.

So the request is built by a projection that replaces a page whose source has been run
again since:

```json
{"success": true, "output": "", "superseded_by": "o9",
 "lines": {"first": 1, "last": 120, "of": 120},
 "buffer": {"id": "o7", "source": "s3f2a1c4"}}
```

- a page goes only when a *newer run* of the same source was answered after it. A run's
  own second page stays, because pages of one log are complementary rather than
  successive, and so does a page the model paged back to on purpose
- the older buffer is still stored, so `superseded_by` comes with the way back: read it
  again by its line number
- the graph is untouched. This happens while the request is assembled, so rewinding,
  saving and reloading all still see the full transcript

Which page came from which run is a fact about a receipt, so it is stored with one: the
session node carries `origin` (`source`, `buffer`, `rank`), written when the result enters
the conversation and read back on load. The projection compares that field instead of
parsing each receipt's text again on every turn — the answer cannot have changed, and
asking it once per turn for a conversation's whole history is how a long session gets slow
in a way nothing points at.

What this does not bound is a conversation that runs many *different* commands, or one
whose prompts and replies grow on their own. Each such run keeps one page, and the pages
accumulate — see [Known Limitations](ARCHITECTURE.md#known-limitations).
