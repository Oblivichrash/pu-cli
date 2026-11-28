# Known Limitations

What this build does not do, or does deliberately in a way a reader would otherwise read
as a bug. Each entry states the limit and why it was chosen, because a limit with no
reason beside it is the first thing someone "fixes" back into the defect it was written
around.

This list used to sit at the end of the architecture document. It is its own
document because it is the one reference here that a reader arrives at with a symptom
rather than a question: someone sees a page that lost its reply, or a server row that will
not go away, and wants to know whether that is the design. A list at the end of a
seven-hundred-line document is a list nobody reads at the moment they need it.

## What no test catches

- **The browser modules are not executed by any test.** Nothing in `web/` is parsed or run by
  `ctest`: this repository has no JavaScript runtime in its toolchain, so a change there is
  checked by reading it and by testing the wire form it consumes, not by running it. What is
  covered is the contract underneath — a serve-stack test asserts the `page` envelope a
  replayed turn receives — and what is not is everything above it: a syntax error, a renamed
  function or a wrong DOM selector ships. The field names a module reads can be traced to their
  writer in `src/`, which is what caught the mismatches so far, but a reader doing that trace
  is the entire gate.
- **The transcript fold is checked by running the page's own JavaScript only where `node`
  exists.** Without it the same case file runs against a second implementation in Python,
  which catches a case that has drifted but not a page that no longer runs.
- **Nothing checks that the C library symbols in a static archive are unique.** Two
  translation units in `pu_model` defined `GatewayName`, `ParseGatewayId` and four others
  identically. GNU ld resolves that silently; MSVC reports `LNK2005` and links nothing. The
  local gate could not have seen it, and until CI ran on Windows nothing did.

## What a turn does that a reader may not expect

- **A turn ends when the model stops calling tools.** There is no per-turn cap on how many
  calls it may make, and nothing tells it that one exists: the loop runs until a reply carries
  no tool call, or until the caller interrupts it.

## What is kept, and how long

- **A cursor does not outlive its buffer.** The pager keeps the last 8 buffers within 4 MiB; a
  page for one already dropped is refused rather than answered from another command's output, so
  the model runs the command again. A cursor names its own run, so the other half of that risk
  does not exist: a page request cannot reach a different command's output and does not have to
  restate the command to prove it. [paging.md](paging.md) owns the shapes.
- **The store is only persisted after a completed interaction and on shutdown.** A crash loses
  everything since the last save, and the store is held in memory in between.

## Delegation

The three entries below are all consequences of one decision: a delegated turn is a second
`Runtime` reading its own directory's `agents.json`, not a continuation of the caller's.
[runtime.md](runtime.md#delegation) owns the mechanics.

- **A delegated turn cannot be asked for approval, and cannot be sandboxed tighter than
  its own file says.** The child `Runtime` never calls `EnableApprovalGate()`, so a tool
  call its directory's `approval_patterns` cover comes back `kNobodyToAsk` even when a
  user is sitting in front of the page: the approval channel belongs to the parent's
  toolbox, not the child's. And the parent's sandbox does not reach into the child at all
  — `Inside()` guards the `ask` string, not the work that follows it, so a directory you
  delegate to is trusted as far as its own `agents.json` allows. Both are deliberate: a
  child that inherited the parent's policy would turn that file into a suggestion instead
  of the boundary.
- **A delegated directory's table is a conversation artifact, not a reviewed one.**
  In the intended use the calling model writes `.pu/agents.json` into the child directory and then
  asks it, so the child's persona, tools, model and sandbox are the caller's text. That is
  deliberately allowed — it is how a purpose-built expert is created mid-turn — but the child's file
  then carries no more trust than the turn that wrote it: configuration for one conversation, not a
  policy anyone reviewed.
- **A stopped or overlong delegated turn is reported, not resumed.** Its answer is
  dropped, and the asking model reads a note saying so; the work the child did is still in
  the child's own turn, which the parent never sees.

## Platform

- **On Windows a silent command cannot be stopped.** `exec::Run`'s Windows reader is a blocking
  `popen` read and notices the stop token only between two reads, so a command that prints
  nothing runs to its own end there. The POSIX reader waits in `poll` and kills the process
  group, which stops the same command on Linux and macOS.
- **Environment probing can come up empty.** An absent `uname` on POSIX yields nothing, and a
  failed Windows kernel API falls back to `"unknown"`.

## The serve registry

- **A server is named by its pid, not by a claim.** A server that is killed
  rather than stopped leaves its line behind, and a pid the operating system later hands to an
  unrelated process reads as a live server until that process exits, so the page can list a
  workspace nothing is serving. Nothing acts on a record beyond drawing it: a wrong row costs a
  wrong name, not a message sent to the wrong place.
- **Two smaller limits come with it.** A machine with no `HOME` gets no list, since there is
  nowhere machine-wide to write it and every workspace's file is the wrong place; and a lock
  that is never released costs the few milliseconds the writer waits before it writes anyway
  rather than costs the server.
