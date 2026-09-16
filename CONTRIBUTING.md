# Contributing

## Pull Requests
- Title format: `type: short description` (e.g. `feat: add tool to ...`)
- Include `## Description`, `## Why`, `## Related Issue` in body.

## Commit Messages
Use a lowercase type and a specific summary under 72 characters. Add a short
list when the change needs context:

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
- Use `clang-format` for formatting; the config lives in `.clang-format`
  (Google style, 100-column limit). Run `clang-format -i <files>` before
  committing. Without the config file, `clang-format` would fall back to the
  LLVM defaults and reformat the whole tree, so always run it from the repo
  root.
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

## Where Things Live

Maintain each fact in one place: rules here, usage and behavior in
[README.md](README.md), structure and layering in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Link instead of copying.

- Build dependencies, configuration, environment variables, and `pu serve`
  usage: [README.md](README.md).
- Layer boundaries, directory layout, CMake targets, and extension points:
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

GPL-3.0 — see [LICENSE](LICENSE)
