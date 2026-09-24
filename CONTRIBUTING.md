# Contributing

## Pull Requests
- Title: the same `<type>: <summary>` form as a commit subject.
- Fill in every section of the [template](.github/pull_request_template.md).

## Commit Messages
Use a lowercase type from this list and a specific summary under 72 characters:

`feat`, `fix`, `refactor`, `perf`, `style`, `docs`, `test`, `build`, `ci`,
`chore`.

Add a short list when the change needs context:

```text
<type>: <summary>

- <important change>
- <important change>
```

Keep one coherent change per commit.

## Code Style
- SPDX license header in every file (`// SPDX-License-Identifier: GPL-3.0-only`).
- Comments carry information: explain **why**, not **what**, and only where the
  reason is not obvious from the code. Describe current intent, not history, and
  do not reference removed code or phrasing like "previously". No decoration
  such as `// =====` banners.
- Format with `clang-format`; the settings live in `.clang-format`. Run
  `clang-format -i <files>` from the repo root before committing — with no config
  in scope the tool falls back to the LLVM defaults and rewrites the whole tree.
- CI runs `clang-format --dry-run --Werror` over every `*.cpp`/`*.hpp` on Linux,
  with the version pinned in
  [.github/workflows/ci.yml](.github/workflows/ci.yml), so a mis-formatted tree
  fails the build.

## Testing
Build first — see [README.md](README.md#build) — then:
```bash
ctest --test-dir build --output-on-failure
```

A change that alters behavior under `src/` needs a test in `tests/unit/` that
fails without it. Documentation-only changes do not.

## Where Things Live

Maintain each fact in one place, and link instead of copying:

- Rules: this file.
- Build dependencies, configuration, environment variables, and `pu serve`
  usage: [README.md](README.md).
- Layer boundaries, directory layout, CMake targets, and extension points:
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

A section owns a fact the same way a field owns a consumer: if nothing reads it,
it does not belong here yet. When a change alters behavior, configuration, or
the file layout, update the document that owns that fact in the same commit; a
stale document is a bug.
