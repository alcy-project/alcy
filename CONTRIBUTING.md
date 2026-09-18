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
uv run ./build/scripts/lint.py
uv run ./build/scripts/format.py --dry-run
uv run ./build/scripts/verify_static_linkage.py

# Or to run all of the above command:
./build/scripts/check.sh

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

CI (`ci.yaml`) runs the `style` job (formatting and typos), the `test-debug`
and `test-release` legs on Linux, macOS, and Windows (standard and Nix
toolchains each), the `dist` packaging legs (release only, intentionally not
gated on the test jobs), and a dedicated `wasm` job, so please make sure all
of them pass before requesting a review.

## WebAssembly builds (compiler playground)

The compiler also builds to WebAssembly via Emscripten (a dedicated `wasm`
job in `.github/workflows/ci.yaml`, Linux-only in CI):

```bash
# Locally (requires emcc and node on PATH):
./build/scripts/check.sh --wasm

# Or directly:
uv run ./build/scripts/run.py --target=tests --mode=debug \
  --build-subdir=build_wasm --target-os=emscripten
```

Notes:

- The wasm build downloads a prebuilt LLVM for `wasm32-unknown-emscripten`
  from the llvm-alcy-fork release matching the submodule tag. If that asset
  does not exist yet, both `check.sh --wasm` and the CI job report it and
  stop (CI stays green until the fork publishes the asset).
- Keep `native` and `wasm` outputs in separate `--build-subdir` directories;
  reusing one output directory across target OSes leaves stale artifacts.
- fpag platform guards use `FPAG_BUILD_FLAG(IS_OS_ASMJS)` for Emscripten-only
  fallbacks (no `execinfo.h`/`dladdr`/module lookup; capped address-space
  reservations). See `docs/build.md` for details.

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
