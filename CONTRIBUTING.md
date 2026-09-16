# Contributing

## Prerequisites

- A C++20 toolchain: Clang, LLD, and libc++ (see [docs/build.md](docs/build.md)).
- GN and Ninja.
- Python via `uv` (`uv sync` sets up the environment; `uv run` prefixes commands).
- Alternatively, Nix provides the whole toolchain: `nix develop`.

## Workflow

Build, test, and check from the repository root:

```bash
uv run ./build/scripts/build.py --target=default --mode=debug
uv run ./build/scripts/run.py --target=tests --mode=debug
uv run ./build/scripts/lint.py
uv run ./build/scripts/format.py --dry-run

```

To automatically fix code style and lint issues:

```bash
# Apply code formatting
uv run ./build/scripts/format.py

# Fix lint issues (clang-tidy, clang-include-cleaner, etc.)
uv run ./build/scripts/lint.py --fix
# Or to also apply fixes that might require manual verification
uv run ./build/scripts/lint.py --fix-errors

```

CI runs the same steps for debug and release on Linux, macOS, and Windows,
plus a spell check (`spelling.yaml`), so please make sure all of them pass
before requesting a review.

## Conventions

- Follow [ARCHITECTURE.md](ARCHITECTURE.md) for module responsibilities and core design principles
  (separation of concerns, YAGNI/DRY/KISS, zero vtables, and zero-allocation hot paths).
  IR construction rules (type currency, operand factories, `SeqBuilder`, opcode
  conventions) live in [docs/ir.md](docs/ir.md).
- Standard: C++20 up to Google C++ Style Guide limits. No exceptions (`-fno-exceptions`).
  Code must not rely on RTTI or EH.
- Naming & Types: `PascalCase` for classes/structs/enums, `kPascalCase` for constants,
  `snake_case_with_trailing_underscore_` for class fields, and `snake_case` otherwise. Use numeric types
  from `"fpag/base/numeric.h"` (`i32`, `usize`, `f64`, etc.) instead of primitive C++ types.
- Code style: Use `#pragma once` for include guards and relative includes from project root.
  Prefer `std::string_view` over `std::string` unless ownership retention is required.
  Avoid magic numbers and prefer designated constructors.
- Comments: English only. Write comments sparingly—only for design rationale,
  invariants/safety explanations, non-obvious code, or `TODO`s.
- Tooling is authoritative: `.clang-format`, `.clang-tidy`, `CPPLINT.cfg` (all checks must pass),
  and `typos.toml` enforce style. Use `format.py` and `lint.py --fix` to fix most issues automatically. 
  New source files must carry the license header.
- One decision, one record: significant technical decisions get an ADR in `docs/adr/`
  (copy `docs/adr/0000-template.md`). Small, obvious changes do not need one.

## Commit scope

Keep changes focused; avoid mixing refactors with behavior changes.
The project is pre-MVP, so APIs and the IR are still allowed to change,
but please call out breaking changes in the commit message.
