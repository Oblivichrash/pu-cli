# Contributing

## Scope

These rules apply to new work: every commit, pull request, and file added from
here on. History is not rewritten to match them — an earlier commit that does
not comply is left as it is.

## Pull Requests
- Title: the same Conventional Commits form as a commit subject.
- Fill in every section of the [template](.github/pull_request_template.md).

## Commit Messages
Follow [Conventional Commits](https://www.conventionalcommits.org/): one
coherent change per commit, an imperative summary within 72 characters, and a
body that lists each change as a `- ` item. A body assembled with one `-m` per
item comes out as separate paragraphs, so pass the whole message at once.

## Code Style
- SPDX license header on every `.cpp` and `.hpp` file
  (`// SPDX-License-Identifier: GPL-3.0-only`). Generated or third-party files
  are exempt.
- A comment carries only what the code cannot say: an external protocol fact, a
  platform `#ifdef` behavior, or an invariant no signature expresses. Everything
  else — intent, rationale, history, structure — is carried by names, types, and
  tests. Do not restate what the next line does, and keep no commented-out code.
- Expect comments to be rare. A block reaching two lines needs a reason to exist
  at all; a file header, where kept, names nothing more than what the file is
  for, and anything longer is a document belonging to the document that
  [Where Things Live](#where-things-live) names for it.
- A comment describes what holds now, and does not point at a document. What an
  earlier build did belongs to the commit that changed it, not to the line that
  replaced it. Design rationale lives in commits and `docs/`, not in the tree.
- Match the style `.clang-format` describes; the settings are there to read and
  to run by hand. Nothing enforces them, so a tree that drifts is not a failed
  build — it is a change to review like any other.

## Testing
A change that alters behavior under `src/` needs a test in `tests/unit/` that
fails without it. Documentation-only changes do not.

- Put the test in `tests/unit/test_<module>.cpp` (one file per module under
  test) and add it to `tests/CMakeLists.txt`. If you add a new file, list it
  there too.
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
| Layer boundaries, directory layout, CMake targets, error hierarchy, persistence, extension points | `docs/ARCHITECTURE.md` |
| What a backend puts on the wire | `docs/providers.md` |
| An external system's contract, or a route considered and not taken | `docs/design/` |

A document in `docs/design/` records what an outside system answers; the code
that follows from it belongs to the files above. A stale document is a bug.
