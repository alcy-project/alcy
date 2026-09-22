# Contributing

## Prerequisites

- A C++20 toolchain: Clang, LLD, and libc++ (see [docs/build.md](docs/build.md)).
- GN and Ninja.
- Python via `uv` (`uv sync` sets up the environment; `uv run` prefixes commands).
- Alternatively, Nix provides the whole toolchain: `nix develop`.

## Workflow

Build, test, and check from the repository root:

```bash
typos
uv run ./build/scripts/build.py --target=default --mode=debug
uv run ./build/scripts/run.py --target=tests --mode=debug
uv run ./build/scripts/check_e2e.py
uv run ./build/scripts/check_runtime.py
uv run ./build/scripts/check_exe.py
uv run ./build/scripts/lint.py
uv run ./build/scripts/format.py --dry-run
uv run ./build/scripts/verify_static_linkage.py

# Or run all of the above commands:
./build/scripts/check.sh

# Faster iteration (skips gn gen, gn check, and compdb; never for CI):
uv run ./build/scripts/build.py --target=tests --fast
```

End-to-end acceptance cases live in `e2e/cases/<name>/` (sources plus
`expect.toml` with the expected exit code and output). The `check_e2e.py`
runner executes the built `alcy` binary against every case; add a case
when a user-visible behavior needs a regression anchor that does not
belong in unit tests.

To automatically fix code style and lint issues:

```bash
# Apply code formatting
uv run ./build/scripts/format.py

# Fix lint issues (clang-tidy, clang-include-cleaner, etc.)
uv run ./build/scripts/lint.py --fix

# Also apply fixes that may require manual verification
uv run ./build/scripts/lint.py --fix-errors

```

Please make sure the CI pass before requesting a review.

```bash
# Locally (requires emcc and node on PATH):
./build/scripts/check.sh --wasm

# Or directly:
uv run ./build/scripts/run.py --target=tests --mode=debug \
  --build-subdir=build_wasm --target-os=emscripten
```

Notes:

- Keep different target OSes outputs in separate `--build-subdir` directories;
  reusing one output directory across target OSes leaves stale artifacts.

## Conventions

- **Architecture:** Follow [ARCHITECTURE.md](ARCHITECTURE.md) for module responsibilities,
  dependency direction, ownership/lifetime boundaries, allocation contracts, and core design
  principles. IR construction rules (type currency, operand factories, `SeqBuilder`, opcode
  conventions) live in [docs/ir.md](docs/ir.md).
- **Standard:** C++20. Follow the Google C++ Style Guide where it does not conflict with
  repository-specific conventions.
- **Explicitness:** Keep significant behavior visible at the call site. Do not use operator
  overloads for domain-specific or non-trivial behavior; use named functions instead. Avoid
  implicit conversions that obscure control flow, ownership, or cost.
- **Ownership:** Make ownership and lifetime explicit in APIs. Prefer non-owning views for
  non-owning relationships and owning types only where ownership is part of the contract.
- **Dependencies:** Respect the dependency direction defined by `ARCHITECTURE.md`. Do not
  introduce dependencies on higher-level modules for convenience, and do not create cyclic
  module dependencies.
- **Naming & Types:** `PascalCase` for classes/structs/enums, `kPascalCase` for constants,
  `snake_case_with_trailing_underscore_` for class fields, and `snake_case` otherwise. Use
  numeric types from `"fpag/base/numeric.h"` (`i32`, `usize`, `f64`, etc.) instead of primitive
  C++ types.
- **Code style:** Use `#pragma once` for include guards and relative includes from project root.
  Prefer `std::string_view` over `std::string` and `std::span` over `std::vector` unless
  ownership retention is required.
- **Comments:** English only. Write comments sparingly-only for design rationale, invariants or
  safety explanations, non-obvious code, or `TODO`s. Do not restate code that is already clear.
- **Tooling is authoritative:** `.clang-format`, `.clang-tidy`, `CPPLINT.cfg`, and `typos.toml`
  enforce repository style. Use `format.py` and `lint.py --fix` to fix most issues automatically.
  New source files must carry the license header.
- **Module naming:** Module names stay abbreviated (`pkg`, `diag`, `cfg`). Directory, GN module,
  and namespace names must always match; full forms live in `ARCHITECTURE.md`.
- **Wording:** The private LLVM fork is `llvm-alcy-fork` on first mention per document,
  and `the fork` thereafter.
- **Architecture decisions:** One decision, one record. Significant technical decisions get an ADR
  in `docs/adr/` (copy `docs/adr/0000-template.md`). Small, obvious changes do not need one.

## Changes and review

Keep changes focused; avoid mixing refactors with behavior changes unless they are inseparable.

When changing architecture, module boundaries, invariants, ownership/lifetime rules, or other
design-level contracts, update `ARCHITECTURE.md` and add or update an ADR when appropriate.

When changing language behavior, update `docs/spec/` in the same change; spec and implementation
must not drift apart.

When changing behavior, tests should cover the new behavior and preserve relevant invariants.

The project is pre-MVP, so APIs and the IR are still allowed to change, but please call out
breaking changes in the commit message.
