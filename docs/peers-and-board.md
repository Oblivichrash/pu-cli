# Peers and the task board

Two `pu serve` instances can talk to each other, and a board carries the work one owes the
other across a restart. This document owns both: the message a peer sends, the token that
lets it in, and the jobs the board holds. The routes those verbs appear on are in
[web-api.md](web-api.md); where the board sits in the process is in
[ARCHITECTURE.md](ARCHITECTURE.md).

> **Frozen.** This block works, is tested, and is not being extended. Nothing in it is
> reachable from one `pu serve` on its own: exercising any verb takes two instances, a
> `peers.json` on each, and a token between them, so it is the one part of the tree that
> cannot be checked by a single command — which means a change here is a change nobody
> notices until it is used. It stays because it is the only working answer this project has
> to "two agents, one job", and removing it would delete the record along with the code.
> Freezing means: no new verbs, no new fields, no new readers on the page, and tests kept
> green. Unfreeze by deciding it is a product direction — and then write down what the
> direction is, because the design that follows from it is not in this document.

## Peer messaging

Two `pu serve` instances can talk to each other, each driven by its own agent and
its own model. A declares B in its peer list and vice versa; either side can then
start a conversation with the other and receive an answer.

```
     ┌─────────────────────────┐                ┌─────────────────────────┐
     │  pu serve (A, :8911)    │                │  pu serve (B, :8912)    │
     │                         │  POST          │                         │
     │  agent calls            │  /api/peer/    │  SessionRunner          │
     │  send_to_peer ──────────┼──message──────►│  ::SubmitFromPeer       │
     │                         │                │         │               │
     │  (returns at once)      │                │         ▼               │
     │                         │                │  a normal turn, run     │
     │                         │                │  on the worker thread   │
     │                         │                │         │               │
     │                         │                │         ▼               │
     │                         │                │  send_to_peer back      │
     │  a new turn             │◄───────────────┼─────────────────────────┘
     │  is queued              │                │
     └─────────────────────────┘                └─────────────────────────┘
```

### Delivery is asynchronous

`send_to_peer` does not wait for the peer to think. It posts a job on the peer's
board, messages that job to the peer, checks that the peer accepted it, and
returns; the asking agent is told plainly that an answer will arrive later. The
peer's reply is injected as a **new turn**, so a slow or busy peer cannot hold up
the sender, and the two agents can work at different speeds.

The job is what makes the wait legible. A reply that arrives as a turn is
invisible until it lands, so without the board neither side could tell a hand-off
that is running from one that was dropped. The receiver claims the job as it
accepts the turn and closes it when the turn ends, which is why a board that
refuses the job stops the call before the message is sent: work the receiver
could not take on must not arrive as a turn.

This is why the tool reports "*has the message. It will answer in a later turn*"
rather than a reply: if it returned the peer's words, the call would block for as
long as the peer's model takes, and a long answer would look like a hang.

### A peer turn may answer only the agent that asked

The gate that stops an endless A→B→A→B chain is that a turn which arrived from a
peer may reach **only its sender**. `send_to_peer` therefore compares the
requested peer against `ToolContext::peer_origin`:

- a turn started by a human has no origin, so every configured peer is reachable;
- a turn started by a peer may reply to that peer, and to nobody else.

Replying to the sender must stay allowed, or a peer could never answer and the
conversation could never close. Relaying onward is refused with a reason the
model can read, so it learns the rule and answers directly instead of retrying.
The chain therefore ends by construction at the second hop: A can ask B, B can
answer A, and that answer cannot be forwarded to a third agent.

Hop counting is deliberately **not** the guard. It is carried for logging only,
because a counter can only guess at intent and a low ceiling would cut off a
legitimate long exchange between the same two agents. Structure, not a number,
decides when the conversation stops.

### Trust boundary

The peer endpoint is the one surface that lets a remote conversation drive a local model,
so it is the one part of the HTTP API that authenticates. The controls, what they do not
cover, and the rest of the trust boundary are in [security.md](security.md).


## The task board

Peer messaging carries one message from one agent to one agent. That is enough to
ask a question and get an answer, and not enough to hand over work: a message has
no state, no owner and no memory of what became of it, so the sender cannot tell a
job that is being worked from one that was dropped, and cannot see that it was
dropped before asking.

The board is the shared object that fixes this. It is a list of jobs, one per line
of work, posted to and read from by every peer and by the human looking at it.

```
   human ──┐
           ├──►  board (jobs, peers)  ◄──►  A   ──►  local model
   A ──────┤          │     ▲
   B ──────┘          │     └── read: "B was last heard from now, and here is what it holds"
                      └──────── read: every open job, every peer's liveness
```

### One owner per job

A job names the peer it is offered to (`to`) and the peer that offered it (`from`).
Only `to` may move it. That is not a permission check bolted on top: it is what
makes the job a responsibility rather than a message. Two peers cannot both be
working the same job, because there is only ever one that can write to it.

The human is not bound by this. They are the scheduler, and their verbs are the
ones no peer has: offer a job, decline on B's behalf, reopen what was declined.

One of those verbs is `Board::Answer` — a person answering the question a blocked
job is waiting on. It sets the answer as the job's `note`, clears the `question`,
and returns the job to `working` for whoever holds it. It is deliberately not
`Update` with a borrowed `from`: a person has no name to present, and letting them
use a peer's would make the audit trail say a peer answered itself.

The other two are `Board::Chase` and `Board::Drop`. A chase is the person asking
a job that has not closed what is happening: it records the note, clears any
`question` (the holder was just asked directly), and counts a `chases`, leaving
the state alone — a job that is slow is not a job that moved, and a state change
would claim it did. A drop closes the job as `cancelled`, the one state no peer
can name: a peer that could say it would be able to end a job it was handed
without doing it. Both reach the holder the same way every other board fact
does, in its digest, so a cancellation that arrives after the work finished
changes nothing.

The page carries all three. The sidebar's board panel is a read-out — it
polls `/api/board` on its own clock, because a peer moving a job is not
something the session's socket reports — and what it writes is these verbs. A
question with no way to answer it would leave the page showing a job that only
another terminal could unblock, and a job nobody can take back is a job a person
watches run past its deadline. The panel draws nothing when the board holds
nothing, so a single-server session carries no empty box.

The read is a separate route rather than `/api/jobs` because of what that route
means: reading it *is* being heard from, and the name it records comes from the
token. A page has no token, and a page polling says nothing about a peer, so
`/api/board` answers the work still owed and the few jobs that settled most
recently, plus the trail, without `peers`, records no one, and is refused to
any request that does not come from the loopback address. It is the one board
route with no credential, and the address is the credential it uses instead.

The split between `open` and `settled` is the server's, not the page's. A page
that decided which states still needed something would hold a second copy of
the state list, and the two would drift the moment a state was added; the
board already knows which jobs have closed, so it answers the two lists and
marks each job `closed`. The settled list is capped at `kMaxSettledShown`,
newest first, because the sidebar is for what is happening rather than a
history — the trail is already the history, and a panel that grew without
bound would push the rest of the column off screen.

**What the design does not build.** Three things a board of this kind usually
carries are deliberately absent, and each stays absent until something needs it:

| Not built | Why not |
| --- | --- |
| Reassign | Only a poster can offer work, and a poster that wants a job elsewhere drops it and posts again; a second route would duplicate the ownership rule |
| Asking a peer for another job after a decline | Declining is already visible on the board as `open` with a `declined_by`, so the scheduler offers it again rather than the board doing it |
| Replication | The poster's copy is authoritative and a holder writes back to it; replicating would buy correctness for the one case where the poster is unreachable, which the person is already watching |

### The lifecycle

```
  open ──claim──► claimed ──start──► working ──┬── done
   │                 │                 │       ├── failed
   │                 │                 │       └── cancelled
   │                 │                 └── block ──► blocked ──unblock──► working
   └──decline────────┘                                     │
   └──reopen───────────────────────────────────────────────┘
```

`done`, `failed` and `cancelled` close the job. Every move on a closed job is
refused, so a finished job keeps its outcome instead of being reopened by a late
write. A chase is drawn beside the moves but is not one: it changes no state, so
a job can be asked about as often as a person likes without the board reading it
as progress. Only a person can bring about `cancelled`; a peer naming that state
is refused as an unknown one.

`blocked` **requires a question**, because the point of the state is to leave
something on the board that a human can answer. A job that stops without stating
what it waits for is indistinguishable from one that was dropped, and the board
would be lying about it being visible.

Declining is not a state. It sets `declined_by` and counts up `declines`, and puts
the job back to `open` where the scheduler can see it and offer it to someone
else — a peer that refuses has not changed the job, only its own relation to it.

### Two clocks, both derived

The board answers two questions that look alike and are not:

| Reading | Question | Derived from |
| --- | --- | --- |
| `overdue` | Has this taken longer than it was given? | `due_at`, set when it was offered |
| `idle_seconds` | Has anything happened here recently? | now − `updated_at` |
| `online` | Is this peer still there? | now − `last_seen` |

Neither is stored. `overdue` and `online` are computed on every read, so a job
goes overdue and a peer goes offline by the passage of time alone; nothing has to
run to notice it, and nothing can be stale.

`due` is given as a **relative duration** at the moment it is offered — `"30m"`,
`"2h"`, `"1d"` — and resolved to an absolute `due_at` by the board. A model can
say how long it expects to need; it cannot reliably say what the current time is.

The two clocks are read differently and lead to different actions. `overdue` asks
the scheduler to decide: chase, reassign, or let it run. `idle_seconds` asks the
same question about a job that was never accepted at all, which is the case that
a deadline alone would miss.

`blocked` and overrunning are **not** messages. They are visible on the board, and
the board is what everyone already reads; sending them would interrupt a peer to
tell it something it can see, and the only replies available are "reassign" or
"intervene", both of which are the human's to make. Results do travel as messages,
because a result is what the sender was waiting for.

### Liveness is observed, not reported

**Reading the board is being heard from.** There is no heartbeat call. A peer
presents its token on `GET /api/jobs`, and the read itself records that peer as
heard from; the same answer carries what the board holds, so one round trip says
"still here" and answers the question at once. The name comes from the token
rather than from the request body: the earlier design took a `peer` field, and a
field is something anyone can write, which made `online` a claim rather than an
observation.

`online` is decided by whoever is looking, from `last_seen`. A peer never says it
is alive; it says when it was last heard from and lets the reader compare that
with its own clock. A peer that is wedged cannot keep claiming to be well, and a
peer whose clock is wrong cannot make itself look healthy.

There is deliberately **no separate heartbeat thread, and no periodic read**. The
pull is made by the turn loop, once per round, because that is the one thread that
is doing the work: a background timer would report the process as alive while the
work is stuck, which is exactly the state that must be visible. Time passes, the
turn stops, the reads stop, and the peer goes dark by itself.

Full visibility is the default: every peer sees every job, not only the ones
addressed to it. A peer that can see what its siblings are holding does not need
to ask, and the human needs the whole board anyway.

### Where the board ends and messaging begins

| Belongs to the board | Belongs to the message channel |
| --- | --- |
| Who holds a job, and what state it is in | The result of a job, once reached |
| What a blocked peer is waiting for | A question to another agent |
| Whether a peer is alive | The conversation that follows |
| That a deadline has passed | Nothing — an overdue job is not urgent enough to interrupt |

A message wakes a peer up; the board is what the peer finds when it looks. The
message channel stays what it was — one exchange between two agents, bounded by
the rule that a peer turn may answer only its sender — and the board carries
everything that has to outlive it.

The one place the two are welded is `send_to_peer`, which posts the job first and
sends second. The ordering is the contract: a message is only ever sent about a
job that is already visible, so the receiver is never told about work that no
board remembers.

### What the model is told, and when

The board is a thing agents read, so it belongs in the context of every turn —
but a digest copied into a prompt is stale by the second round of the tool loop.
`Runtime::SetContextDigest` hands the executor a function rather than a string,
and `Executor::BuildStaticSystemContext` calls it on every round. A job claimed
mid-turn therefore reads as claimed on the next round, without the model asking.

`Board::Digest(self_name)` answers with two lists and nothing else:

| List | Why the model needs it |
| --- | --- |
| Jobs this server holds, open or closed | It is the only one who can move them; the digest is where it learns that |
| Jobs this server handed out that have closed | The outcome it was waiting for, delivered without a message or a poll |

Each held job carries the peer it came from, the intent, its state, any question
it is being asked, whether the deadline has passed, how many times a person has
asked about it, and what that person last said. The note line matters because a
chase is how a person steers a job without moving it: the count says it was
asked, the note says what was wanted, and the holder is the only reader that can
act on either. A closed job carries its result. Nothing is listed that the model
cannot act on.

An empty board yields an empty digest, and an empty digest adds nothing, so a
session with no peers in play sends exactly the request it sent before the board
existed.

### Where the authoritative copy lives

There is no global board. The server that *posted* a job holds the copy that
counts, and the peer working it writes back to that server's `/api/jobs`. A job
therefore belongs to the conversation between two servers, not to a shared
store: no coordination, no leader, and nothing to reconcile while every server is
reachable.

What this costs is a job that outlives its poster's reachability: if the poster's
server stops, the job it holds stops with it, and the holder's updates are
refused until it returns. That is accepted rather than papered over, because the
alternative — replicating jobs — buys correctness for a case the human is
already watching.

---
