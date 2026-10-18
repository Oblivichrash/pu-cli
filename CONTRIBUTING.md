# Contributing

## Scope

These rules apply to new work: every commit, pull request, and file added from
here on. History is not rewritten to match them — an earlier commit that does
not comply is left as it is.

## Pull Requests
- Title: the same Conventional Commits form as a commit subject.
- Fill in every section of the [template](.github/pull_request_template.md).
- A `Co-authored-by:` trailer is a record of a real co-author, never a
  requirement: no trailer is expected on a change you wrote alone.

## Commit Messages
Follow [Conventional Commits](https://www.conventionalcommits.org/): one
coherent change per commit, an imperative summary within 72 characters, and a body
that says why.

- Bullet points are the default shape of the body: each starts with a capital
  letter and ends without a period. A short paragraph is equally acceptable where a
  list would read as fragments — a fix that needs one sentence of context is one
  sentence, not a list of one.
- Reason, trade-offs and history belong in the body. Design rationale that outlives
  the change belongs in `docs/`.
- A change that cannot be justified in this form is usually more than one commit.

```
<type>: <imperative summary>

- <change>
- <change>
```

## Code Style
- SPDX license header on every `.cpp` and `.hpp` file
  (`// SPDX-License-Identifier: GPL-3.0-only`). Generated or third-party files
  are exempt.
- Match the surrounding code: indentation, brace placement, include order and line
  width are what the file you are editing already does. Nothing checks formatting,
  so a tree that drifts is a change to review like any other rather than a failed
  build.
- A comment explains why the code is the way it is: an external protocol fact, a
  platform `#ifdef` behavior, or an invariant no signature expresses. It does not
  restate what the next line does, and no commented-out code is kept.
- A public header may carry a short doc comment on a class or function whose
  contract is not obvious from its name. A link to the document that owns a rule —
  `// see docs/providers.md` — is welcome where it saves the reader a search; a
  comment that paraphrases the document is not.
- A comment describes what holds now. What an earlier build did belongs to the
  commit that changed it, not to the line that replaced it.

## Testing
A change that alters behavior under `src/` needs a test in `tests/unit/` that
fails without it. Documentation-only changes do not.

- One file per module under test: `tests/unit/test_<module>.cpp`. A new module gets
  a new file; a change to an existing module adds cases to the file that is already
  there rather than starting a second one. Every file is listed in
  `tests/CMakeLists.txt`.
- Register cases with Catch2's `TEST_CASE`, tagged with the module name (e.g.
  `"[executor]"`). `catch_discover_tests` already registers them with `ctest`,
  so a case that compiles is a case that runs.

## Where Things Live

Each fact has one home — link to it instead of copying it. When a change alters
one of these, update the document in the same commit:

| What changes | Document that owns it |
|--------------|-----------------------|
| Rules (this file) | `CONTRIBUTING.md` |
| Build, configuration, environment variables, `pu serve` usage | `README.md` |
| `agents.json`, `peers.json`, environment variables, logging | `docs/configuration.md` |
| Web UI and API: WebSocket messages, REST endpoints | `docs/web-api.md` |
| Layer boundaries, directory layout, CMake targets, error hierarchy, persistence, extension points | `docs/ARCHITECTURE.md` |
| What a backend puts on the wire | `docs/providers.md` |
| Release history | `CHANGELOG.md` |
| An external system's contract, or a route considered and not taken | `docs/design/` |

A document in `docs/design/` records what an outside system answers; the code
that follows from it belongs to the files above. A stale document is a bug.

Conventions:

- Probed findings that are expected to drift — which fields a gateway honors, which
  model ids answer — go in a `*-fieldnotes.md` beside the contract and say plainly that
  they are observations rather than requirements. That is what keeps the contract
  document able to be stale-proof.
- Adding a file to `docs/` means adding a row to the table above in the same commit,
  so a new fact has exactly one home from the day it is written.
