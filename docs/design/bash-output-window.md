# Paging a command's output

This records why `execute_bash` answers with a page, the routes that were not taken,
and what the shape gives up. What is built is stated in
[README](../../README.md#execute_bash-pages-its-output) and
[ARCHITECTURE](../ARCHITECTURE.md#executor); this document carries the reasoning.

**Built:** a command's output is captured within a byte budget, kept in a
session-scoped pager, and read one contiguous page at a time through a cursor; the
budget is spent on the end the call names (`head` or `tail`).

**Names.** The four nouns below are types now: `paging::Buffer`, `paging::Page`,
`paging::Cursor`, `paging::Pager`, with `paging::Anchor` for "where a read starts" and
`exec::Run` for the capture. `paging/paging.hpp` and `exec/run.hpp` are the whole
surface; what follows is why it has that shape.

---

## 1. The problem

Before this, `execute_bash` returned everything the command printed, and the only
limit on that was a sentence in the injected system prompt: *use `head -n 50`,
`tail -n 50`, `grep`, `sed -n '10,30p'` instead of full cat dumps*. Three things are
wrong with a rule that lives there:

- It is not enforced. One `cat` of a generated file, one `find /`, one `git log`
  without `-n`, and the turn's context is spent on output nobody will read.
- It is not testable. Nothing can assert that the model followed it.
- It cannot be retried. The output has already been stored verbatim in the
  transcript by the time the next turn sees it; there is no "read that again, from
  the top".

The size of the problem is a property of the command, not of the tool: `ls` prints
20 lines and `cat access.log` prints a million, and the tool treats them the same.

## 2. What a terminal does, and which part of it is worth copying

A person reading a file in a shell is doing three things this tool did not:

1. **Seeing one screen, not the file.** The screen is a window over a buffer, and the
   buffer is what the pager holds.
2. **Choosing an end before choosing a page.** `less file` starts at the top;
   `tail -f log` starts at the bottom; nobody starts in the middle.
3. **Reading on from where they stopped, without asking the producer again.** The
   bytes are already on the pager's side of the pipe.

Two parts of the terminal are deliberately *not* copied:

- **A page arrives after the command finishes, not while it runs.** A real pager
  applies backpressure to the producer — `head` closes the pipe and the producer
  takes `SIGPIPE`. Here the command runs to its own end and the excess is counted
  rather than kept, because a reader that walks away turns a command's exit code
  into a signal, and `exit_code` is what the model uses to decide whether the
  command worked.
- **There is no interactive session.** Each page is one tool call and one turn's
  worth of tokens; the model cannot scroll, so the only state it has is the cursor
  it writes down.

## 3. The shape

```
Capture      the byte budget and which end it is spent on
Buffer       the lines that were kept, plus the command's verdict
Page         a contiguous run of those lines
Cursor       <buffer>:<line> — the pair the next call names
Pager        the buffers still readable, oldest dropped first
```

The pieces are split this way because they fail differently. The capture is the only
part that touches a pipe, the page is the only part that does arithmetic on lines,
and the pager is the only part that forgets — so each can be tested without the
others, and the pager can be reasoned about as a lifetime question alone.

**The budget follows the end.** A log's newest lines are what a reader wants and a
file's opening lines are what an editor wants, and one budget cannot be spent on
both. `view` is passed down to the capture, which keeps a prefix or a suffix — the
same choice `head` and `tail` make in the shell. A suffix capture counts the lines
it dropped from the front (`skipped_lines`) so a page can still say *line 3505 of
3520* rather than numbering the buffer it happens to hold.

**A page says what it cannot reach.** `retained_lines` is what the buffer kept and
`total_lines` is what the command printed; `truncated` is the difference being
non-zero. `earlier` and `later` are cursors, and a missing one means no page lies
that way *within the buffer* — which, when `truncated` is set, is the honest answer
rather than a promise that nothing exists there.

**A cursor is data, not a session.** It names a buffer and a line, and the buffer
names the command that produced it. A cursor offered with a different `command` is
refused instead of silently paging the other command's output: a model that reworded
its command meant a different output, and the refusal says which command the cursor
does belong to.

## 4. Routes not taken

- **Trim the output after capturing all of it.** The context would be protected and
  nothing else would be: a `find /` would still build a 2 GiB string in memory to
  hand back a page of it, and the page could not be left and returned to. The budget
  is applied while reading for that reason.
- **Re-run the command with `sed -n`/`awk` for the next page.** Cheapest to build,
  and wrong wherever the command is not a pure function of the filesystem: a log that
  grew, a build that changed, a `ps` that answers differently. A page of a run that
  already happened should be read from that run.
- **A head+tail digest instead of pages.** One call, one screen, both ends. It
  answers "what does this look like" and nothing else — the middle is unreachable,
  and the model's next move is the same `sed`-by-hand paging the digest was meant to
  replace.
- **Choosing head or tail inside the tool.** The intent belongs to the call — a file
  about to be edited is read from the top and a log from the bottom, and only the
  model knows which it is looking at. A heuristic on the command string would be a
  second opinion the model cannot see and cannot override, and the shell already has
  the heuristic in the shape of `head`/`tail`/`grep`.
- **Killing the producer when the budget is reached.** It is what `head` does, and
  it makes `exit_code` mean *the reader left* rather than *the command failed*. The
  command is left to finish.
- **Keeping both ends of a truncated capture.** It reads well until a page has to
  cross the gap the buffer does not hold: a cursor that names a line the buffer never
  had turns one lie into two. One end, stated plainly, is the smaller failure.
- **A pager backed by files.** Survives a restart and costs no memory, but adds a
  directory to clean, a permission question, and a write on every command whose
  output happened to be large. The pager lives in the session instead, and a cursor
  from before a restart is refused like any other expired one.

## 5. What this does not solve

- **A page can still be too big.** `max_lines` bounds it, so raising it to 5000 puts
  the overflow back; the tool only guarantees that the *default* is small.
- **A turn is bounded per source, not in total.** The projection replaces the page of
  a run that the same source has since been run again to supersede, so paging one log
  ten times costs one page. Thirty *different* commands still cost thirty pages: the
  bound is per question, and nothing yet bounds the number of questions. A per-turn
  budget over tool results is the missing half, and it belongs in the projection
  beside this, not in the pager.
- **A command that is slow is not made faster.** Nothing is read until the command
  finishes; paging bounded the output, not the wait.
- **Eviction can still strand a receipt.** Buffers last as long as the runtime, but a
  buffer dropped for age or size leaves its receipt pointing at nothing. The tool says
  so ("cursor has expired") rather than inventing output, and pinning a buffer some
  live message refers to is the fix that is not written yet.

## 6. See also

- [context-viewport.md](context-viewport.md) — the general shape this is an
  instance of: *(source, anchor, budget)* instead of *(command, line, page)*, the
  salience sidecar, and how a terminal screen becomes a source.
- [native-library-calls.md](native-library-calls.md) — a second producer of the
  same problem, where a call's result is a handle that is inspected through a
  window rather than copied into the transcript.
