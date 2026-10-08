# Contributing

## Pull Requests
- Title: the same Conventional Commits form as a commit subject.
- Fill in every section of the [template](.github/pull_request_template.md).

## Commit Messages
Follow [Conventional Commits](https://www.conventionalcommits.org/) with an
imperative summary within 72 characters, and keep one coherent change per
commit.

## Code Style
- SPDX license header in every file (`// SPDX-License-Identifier: GPL-3.0-only`).
- Comments carry information: explain **why**, not **what**, and describe current
  intent rather than history. No decoration such as `// =====` banners.
- Format with `clang-format`; the settings live in `.clang-format`. Run
  `clang-format -i <files>` from the repo root before committing — CI checks the
  formatting, so an unformatted tree fails the build.

## Testing
A change that alters behavior under `src/` needs a test in `tests/unit/` that
fails without it. Documentation-only changes do not.

## Where Things Live

Each fact has one home — link to it instead of copying it:

- Rules: this file.
- Build, configuration, environment variables, and `pu serve` usage:
  [README.md](README.md).
- Layer boundaries, directory layout, CMake targets, and extension points:
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- What each backend puts on the wire:
  [docs/providers.md](docs/providers.md).
- External contracts, and routes considered and not taken:
  [docs/design/](docs/design/). A document there records what an outside system
  answers; the code that follows from it belongs to the files above.

When a change alters behavior, configuration, or the file layout, update the
document that owns it in the same commit. A stale document is a bug.
