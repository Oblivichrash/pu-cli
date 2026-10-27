# Calling a C library directly

The proposal: the model should call native libraries in-process instead of shelling
out, because the system already has many such libraries, they are fast, and their
interfaces are public so nothing has to be documented by hand.

The first half is right and worth building. The second half is the part that decides
whether this is a feature or a foot-gun, so it comes first.

---

## 1. A header is a signature, not a contract

`clang` can tell you that `int compress(Bytef *dest, uLongf *destLen, const Bytef
*source, uLong sourceLen)` takes two pointers, two lengths and returns an `int`.
That is the easy 60%. What the model needs in order not to corrupt memory is in the
other 40%, and none of it is in the signature:

| Question | `compress` | `sqlite3_open` | `strtok` | `getenv` |
|---|---|---|---|---|
| who owns the returned pointer | — | `sqlite3_free` | static, next call invalidates | static, do not free |
| how many bytes may be written to `dest` | `*destLen` in, `*destLen` out | — | — | — |
| what "failed" looks like | return `Z_BUF_ERROR`, read `errno`? no — a zlib code | return code, never `errno` | `NULL` | `NULL` means *absent*, not *error* |
| thread safety | reentrant | per-handle, not per-library | global state | reads the environment |
| blocking | no | yes (I/O, locks) | no | no |
| state carried between calls | none | the handle | the cursor | none |

`assert(len > 0)` is not in the header either, and the model *will* pass 0 — not out
of malice, but because a plausible-looking argument is what a language model
produces. Without a declared precondition, a hallucinated integer becomes memory
corruption in the agent's own address space.

So the binding is not a description. **It is a validator plus a marshal plan**, and
the parts that cannot be derived are per *library* (defaults) rather than per
function (exceptions). That is the difference between "maintain docs for 400
functions" and "maintain 30 lines per library": derive the signatures with libclang,
write down only what the header omits.

## 2. Why do this at all — and why not for speed

| Path | Cost per call |
|---|---|
| `fork` + `exec` + shell parse + text decode | ~2–15 ms (POSIX), ~10–40 ms (Windows `CreateProcess`) |
| `dlsym` thunk, warm | ~0.1–2 µs + marshal |
| RPC to a warm worker process | ~50–200 µs |
| one LLM turn | 500–5000 ms |

The call is 3–5 orders of magnitude cheaper than the turn that requested it.
Optimising the call is therefore measuring the wrong thing; the costs that actually
dominate are **round trips** and **tokens**. The real wins of native calls are
qualitative:

- **No text boundary.** A `double` stays a `double`. A struct does not have to be
  printed in a format the model then re-parses with a regex. A `char*` that contains
  a null byte survives.
- **No shell quoting.** Arguments stop being a string that `sh` re-lexes. Today
  every command is a small injection surface and an escaping bug.
- **Binary data stays binary.** A 40 MB decoded image does not become base64 in a
  JSON string, then into the transcript.
- **Handles instead of values.** The result of call one can be *referenced* by call
  two rather than serialised through the model. This is the same move the viewport
  makes for output: keep the data where it is, pass a name for it.

Given that, the design should spend its complexity on isolation and validation, not
on shaving microseconds.

## 3. Execution model: out of process, warm, killable

In-process `dlsym` is the obvious implementation and the wrong default:

- a segfault in the library kills the agent and the session;
- a blocking call cannot be cancelled — C has no portable interruption point, and a
  signal handler that longjmps out of a library is undefined behaviour;
- a leak or global mutation is permanent for the rest of the session;
- a bad pointer passed by the model corrupts the harness's own heap.

So: a **worker process** per library, holding the `dlopen`/`LoadLibrary` handle,
speaking a length-prefixed request/response protocol on a pipe. Two modes:

| Mode | Mechanism | State | Crash cost |
|---|---|---|---|
| `isolated` (default) | POSIX: `fork()` per call, call in the child, pipe the result back. Windows: spawn per call | none between calls | the child dies, the answer is an error |
| `stateful` | one long-lived worker; opaque handles index into its tables | kept | restart the worker, handles become invalid |

`fork` is the right primitive on POSIX because copy-on-write makes it ~0.5 ms and it
gives crash isolation for free; a library that must keep state (a database handle, a
decoder context) opts into `stateful` and accepts that a crash loses the state. On
Windows there is no `fork`, so `isolated` is a process spawn and the binding should
say whether the cost is acceptable (it usually is; the alternative is `stateful`).

The worker is where the safety work happens: it runs under a job object / rlimit,
with a wall-clock timeout enforced by the parent (kill the process; there is no
polite way to stop native code), a memory cap, and — on Linux — a seccomp filter
that denies `execve`, `socket`, and `open` outside the sandbox root. A library call
that needs to open a file gets it opened by the harness and passed as an fd, not by
`fopen` with a model-supplied path.

## 4. The tool surface: three calls, not one

Tool schemas are context. A catalog of 400 native functions injected into the system
prompt is the thing this whole design is trying to avoid, so the surface is
**progressive**:

```
lib.list    {query?}          -> [{library, summary, symbols: n}]      ~20 lines
lib.describe {library, symbol?} -> schema for one symbol, on demand
lib.call    {library, symbol, args, timeout_ms?} -> {result, handles, errno?...}
```

`lib.list` takes a query because "which of these can decode a PNG" is the actual
question; a flat list of every installed `.so` is not an answer. `lib.describe`
returns the *validated* schema — types, preconditions, ownership, error convention —
which is the only text the model sees, and only for the symbol it is about to call.

`lib.call` accepts a batch: an array of calls executed in order in one round trip,
with results by index. Ten calls in one turn is where the efficiency actually comes
from; ten separate tool calls pay ten LLM round trips for ten microseconds of work.

## 5. The binding format

```json
{
  "library": "libz.so.1",
  "abi": "c",
  "defaults": {
    "thread": "safe",
    "blocking": false,
    "error": {"kind": "code", "ok": 0},
    "ownership": "caller"
  },
  "symbols": {
    "compress": {
      "args": [
        {"name": "dest", "type": "out_buffer", "capacity_from": "destLen"},
        {"name": "destLen", "type": "inout_u64", "min": 1},
        {"name": "source", "type": "in_buffer"},
        {"name": "sourceLen", "type": "u64", "max_from_len": "source"}
      ],
      "returns": "i32",
      "error": {"kind": "zlib_code", "ok": 0},
      "pure": true
    },
    "compressBound": {
      "args": [{"type": "u64"}],
      "returns": "u64",
      "pure": true
    }
  }
}
```

What each part buys, in order of importance:

- `capacity_from` / `min` / `max_from_len`: **the validator**. The marshal layer
  refuses a call whose declared buffer is smaller than the length that will be
  written. This is the single highest-value field in the file.
- `ownership`: who frees a returned pointer. `caller` | `library` (with the free
  function named) | `static` (copy immediately, never free) | `invalid-after-call`
  (e.g. `strtok`). Getting this wrong turns a correct call into a double free twenty
  calls later, which is unreproducible.
- `error`: `code` (return value), `errno` (must be cleared before the call and read
  immediately — it is thread-local and the next libc call may clobber it), `out`
  (an out-parameter), `sentinel` (`NULL`/`-1`).
- `pure`: whether the same arguments always give the same answer. Only `pure`
  symbols may be cached, and caching is the one optimisation that does pay, because
  it removes calls rather than shortening them.
- `blocking`: whether the call may block. A blocking symbol in `isolated` mode is
  fine; in `stateful` mode it stalls every other caller of that worker, so the
  worker needs a queue and the binding needs to say so.

**Type vocabulary, deliberately small:**

`i8..i64`, `u8..u64`, `f32/f64`, `bool`, `cstring` (NUL-terminated UTF-8 in),
`out_cstring`, `in_buffer` (ptr+len), `out_buffer` (ptr+capacity), `handle`
(opaque, harness-minted), `struct_ptr` (layout from libclang: offsets, size,
alignment). That is enough for the great majority of C libraries and it is small
enough to validate, fuzz and test.

**Excluded on purpose:** variadic calls (`printf` family), callbacks (a C function
pointer the model cannot supply — a small set of harness-provided trampolines can be
added later, gated), by-value aggregates (passing a struct in registers differs
between SysV and MSVC, and `libclang` layout does not settle the calling
convention), `setjmp`/`longjmp`-based error paths, and anything that installs a
signal handler or calls `fork`/`atexit`.

**C++ is out of scope.** Mangled names, `std::string` ABI, exceptions crossing the
boundary, and no stable layout. If a C++ library matters, the wrapper is a small
`extern "C"` shim — ten lines, reviewed once, and then it is a C library like any
other.

## 6. Data that should never travel through the model

A returned 40 MB buffer has three possible destinations, and only one is right:

1. into the tool result as base64 — never; it costs megabytes of context for a value
   the model cannot read anyway;
2. to a file, then re-read through the viewport — acceptable, but pays a disk round
   trip and leaves litter;
3. **as a handle** — `{"handle": "h7", "bytes": 41943040, "kind": "image/png"}`,
   with `describe`/`window` letting the model look at a slice, and `write` letting it
   persist if it actually wants the bytes.

The handle table is the junction between this document and
`context-viewport.md`: a handle is a *source*. `view.open(source: "h7", anchor:
{"offset": 1024})` is how a model inspects a 40 MB buffer without receiving it, and
it is the same machinery as paging a command's output. Design them together or the
handle table grows its own half-pager.

Small returns (a status code, a 12-byte struct, a short string) go in the result
directly. The threshold is a config value, default 4 KiB, and the truncation is
stated in the envelope — same discipline as `truncated` on a capture.

## 7. The second tier: one call, many operations

Batching (§4) amortises round trips but cannot express a loop. The common shape is
"for each of 900 files, call `md5`", and the model would have to write 900 calls.

Two ways out, and they are genuinely different bets:

| Route | How | Cost | Risk |
|---|---|---|---|
| **A. Declarative batch** | the model supplies an array; harness loops | tiny, stays validated | no conditionals, no derived arguments |
| **B. Sandboxed script** | the model writes a short script in a worker with pinned bindings (LuaJIT FFI, or a C-subset interpreter) | a whole language runtime | arbitrary memory access inside the worker; needs seccomp + a real sandbox |
| **C. Codegen + compile** | the model writes C, harness compiles and runs it | compiler dependency, slow first call | same as B, plus a toolchain |

Recommendation: ship **A** with the first version, and treat **B** as the next tier
behind a config flag. B is what makes the feature feel native rather than like a
long list of RPCs, but LuaJIT's `ffi.cdef` is an unbounded memory-write primitive —
it is only acceptable because the worker is already disposable and seccomp'd, and
because `execute_bash` is already arbitrary code execution. Do not pretend B is
safer than the shell; make it *not worse*.

C is rejected for now: pulling a compiler into the runtime to save a round trip is a
large dependency for a small latency win, and it makes every call's failure mode a
build error the model has to iterate on.

## 8. Safety: what the policy has to say

Extend `SecurityPolicy` rather than inventing a parallel gate:

```json
"native": {
  "allow": [
    {"library": "libz.so.1", "symbols": ["compress", "uncompress", "compressBound"],
     "mode": "isolated"}
  ],
  "max_bytes_out": 4194304,
  "timeout_ms": 5000
}
```

- **The model never names a path.** It names a library *from the allowlist*; the
  harness owns the path. `dlopen` with a model-supplied path is `LD_PRELOAD` with
  extra steps. On Windows, `LoadLibraryExW` with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`; never bare
  `LoadLibrary`, which searches the current directory and is the classic planting
  vector.
- **First call to a library asks the user**, through the existing `Approval` hook.
  A new library is a new capability, and the pattern (`approval_patterns`) already
  exists for commands.
- **Pin what was loaded.** Record build-id (ELF) / PE timestamp+size and the
  resolved absolute path in the call log. A library upgraded under a running session
  changes the ABI of a live handle; the log makes that diagnosable rather than
  mysterious.
- **Deny by symbol, not by library, where it matters**: `system`, `popen`, `exec*`,
  `dlopen`, `setenv`, `ptrace`. A library that happens to export them is not an
  escape hatch.
- **The worker is the sandbox boundary**, not the call site. Assume every call can
  corrupt the worker and design for the worker dying.

## 9. Testing

One tiny fixture library in CI, `libputest`, with symbols that are each a failure
mode: `add(i32,i32)`, `echo(const char*)`, `fill(u8*, len, byte)`,
`out_param(i32*)`, `return_null()`, `set_errno_and_fail()`, `write_oob_1byte()`,
`spin_forever()`, `segfault_now()`, `leak_1mb()`. The assertions that matter are not
"it returns 3"; they are:

- a crash is reported as a tool error, and the *agent process survives*;
- a timeout kills the worker and says so, rather than hanging the turn;
- a call that would write past the declared capacity is refused **before** the
  call, with the capacity and the required length in the message;
- a handle from a restarted worker is refused, not dereferenced.

Run the worker under ASan in CI. A marshal layer that only ever sees good arguments
is untested.

## 10. Roadmap

| Phase | Content | Why this order |
|---|---|---|
| 0 | `SecurityPolicy.native` + allowlist parsing | the gate must exist before the capability |
| 1 | worker + `lib.list` / `lib.describe` / `lib.call`, scalars, `cstring`, `in_buffer`, `out_buffer` | the 80% of useful C libraries |
| 2 | handles + viewport integration, batch calls | where the token savings are |
| 3 | libclang-driven schema derivation + per-library defaults file | removes the "maintain docs" objection |
| 4 | `stateful` worker, `pure` caching | for handles and hot paths |
| 5 | sandboxed script tier (B) | amortises loops; needs the sandbox from phase 1 |

Phase 3 is the answer to the original premise: signatures *can* be derived
automatically, so the maintained artefact is a per-library defaults file with
per-symbol exceptions, not documentation. What cannot be derived is the contract,
and that is exactly the part that must not be guessed.

## 11. Routes not taken

- **In-process for latency.** The margin over a turn is four orders of magnitude;
  the price is that any model-supplied bad pointer takes the session with it.
- **Raw `dlsym` by name from the model, no manifest.** It is `execute_bash` with
  worse error messages and no policy.
- **Expose every symbol of every library, generated from headers.** 40 000
  signatures in a tool schema, 90% of them irrelevant, and no validation. This is
  the failure mode the viewport work exists to prevent, applied to tools instead of
  output.
- **Wrap known libraries behind bespoke tools** (`image.decode`, `pdf.text`). Good
  UX, and the right thing for a handful of hot libraries — but it does not scale to
  "many libraries", which is the premise. Do both: bespoke tools for the top five,
  `lib.call` for the tail.
- **Embed a Python interpreter and let the model import anything.** CPython's C
  extension surface is larger than the FFI surface, with the same memory hazards and
  a much bigger runtime. LuaJIT with explicit `cdef`s (tier B) is the smaller bet.
