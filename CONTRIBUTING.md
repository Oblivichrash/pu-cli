# Contributing

## Pull Requests
- Title format: `type: short description` (e.g. `feat: add tool to ...`)
- Include `## Description`, `## Why`, `## Related Issue` in body.

## Commit Messages
Use a lowercase type from this list and a specific summary under 72 characters:

`feat`, `fix`, `refactor`, `perf`, `docs`, `test`, `build`, `ci`, `chore`.

Add a short list when the change needs context:

```text
<type>: <summary>

- <important change>
- <important change>
```

Keep one coherent change per commit.

## Code Style
- C++23 with Google C++ Style.
- SPDX license header in every file (`// SPDX-License-Identifier: GPL-3.0-only`).
- No decorative comments (`// ====`).
- Comments explain **why**, not **what**. Prefer self-explanatory code: add a
  comment only when the reason is not obvious from the code itself.
- Comments describe current intent, not history. Do not reference removed
  code, old versions, or phrasing like "previously" / "it used to".
- Use `clang-format` for formatting; the settings live in `.clang-format`. Run
  `clang-format -i <files>` before committing. Without the config file,
  `clang-format` falls back to the LLVM defaults and reformats the whole tree,
  so always run it from the repo root.
- CI runs `clang-format --dry-run --Werror` over every `*.cpp`/`*.hpp` on Linux,
  with the version pinned in
  [.github/workflows/ci.yml](.github/workflows/ci.yml), so a mis-formatted tree
  fails the build.
- JSON code uses Boost.JSON (`boost::json::value`) through the
  `include/pu/core/json.hpp` helpers (`pu::json::parse`, `pu::json::serialize`,
  `pu::json::ValueOrDefault`, `pu::json::HasKey`,
  `pu::json::PrettyPrint`) instead of raw hand-rolled parsing.
- A header belongs in `include/pu/` only when code outside its own directory
  uses it (including tests); otherwise keep it next to its `.cpp` under `src/`.

## Testing
Install the dependencies from [README.md](README.md#build-dependencies), then:
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
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

## License

GPL-3.0 — see [LICENSE](LICENSE)
