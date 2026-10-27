# Paged tool output

A command can print more than a turn can carry. `execute_bash` therefore answers with one
page of what the command printed, and keeps the rest where a later call reads it without
running the command again.

## What one call returns

Beside the usual `success`/`output`/`error`/`exit_code`, a page carries three scopes:

```json
"page":   {"first": 3401, "last": 3520, "earlier": "o7:3281", "later": null},
"buffer": {"id": "o7", "source": "s3"},
"extent": {"lines": 3520, "retained": 240, "truncated": true}
```

- **`page`** is where this page sits. `first`/`last` number the lines of the command's
  whole output, so a page says where it is even when the buffer holds one end of it.
  `earlier`/`later` hold a cursor where a page lies that way — `earlier` from a tail page
  is how a log is read backwards.
- **`buffer`** is which run produced it. `id` names the run and is what a cursor points
  at; `source` names the question — the command and the directory — so two runs of one
  command share a source and differ in id.
- **`extent`** is how much of the run survived: `lines` what the command printed,
  `retained` what the buffer kept, `truncated` whether they differ. Paging can reach
  `retained` and no further, which is why a truncated capture is a different statement
  from a long one.

`at` says where to read: `head` (the default), `tail`, or a cursor. `limit` asks for a
number of lines, clamped to `max_lines`.

## A cursor needs no command

`at: "o7:3281"` reads that page. The cursor already names the run, and the run already
knows its command, so restating a 200-character command to read page two would be a
second source of truth that can only disagree with the first.

Two limits follow from the store being a store:

- **A page request also names the command it belongs to.** A cursor from a different
  command is refused rather than answered from the wrong output.
- **A cursor expires only when its buffer is dropped** — the last 8 runs within 4 MiB,
  both configurable under [`paging`](configuration.md#paging). The buffers belong to the
  runtime, not to a tool, so a cursor still pages after the toolbox is rebuilt, which is
  what switching model or agent does.

## A turn keeps one page per source

Every tool result is stored in the conversation and sent again on every later turn. A page
left as it was would therefore cost its lines again and again, and a model that pages ten
times would fill a context with nine pages it has already read.

So the request is built by a projection that replaces a page whose source has been run
again since:

```json
{"success": true, "output": "", "superseded_by": "o9",
 "page": {"first": 1, "last": 120, "earlier": null, "later": "o7:120"},
 "buffer": {"id": "o7", "source": "s3"}}
```

- a page goes only when a *newer run* of the same source was answered after it. A run's
  own second page stays, because pages of one log are complementary rather than
  successive, and so does a page the model paged back to on purpose
- the older buffer is still stored, so `superseded_by` comes with the way back: page it
  with the `earlier`/`later` cursor from its own `page`
- the graph is untouched. This happens while the request is assembled, so rewinding,
  saving and reloading all still see the full transcript

What this does not bound is a conversation that runs many *different* commands, or one
whose prompts and replies grow on their own. Each such run keeps one page, and the pages
accumulate — see [Known Limitations](ARCHITECTURE.md#known-limitations).
