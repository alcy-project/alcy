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

- **Architecture:** Follow [docs/architecture.md](docs/architecture.md) for module responsibilities,
  dependency direction, ownership/lifetime boundaries, allocation contracts, and core design
  principles. IR construction rules (type currency, operand factories, `SeqBuilder`, opcode
  conventions) live in [docs/ir.md](docs/ir.md).
- **Standard:** C++20. Follow the Google C++ Style Guide where it does not conflict with
  repository-specific conventions.
- **Explicitness:** Keep significant behavior visible at the call site. Do not use operator
  overloads for domain-specific or non-trivial behavior; use named functions instead. Avoid
  implicit conversions that obscure control flow, ownership, or cost. Avoid hardcoded values
  (unnamed numbers or strings) in domain logic; bind them to meaningful constants.
- **Ownership:** Make ownership and lifetime explicit in APIs. Prefer non-owning views for
  non-owning relationships and owning types only where ownership is part of the contract.
- **Dependencies:** Respect the dependency direction defined by `docs/architecture.md`. Do not
  introduce dependencies on higher-level modules for convenience, and do not create cyclic
  module dependencies.
- **Naming & Types:** `PascalCase` for classes/structs/enums, `UPPER_SNAKE_CASE` for constants,
  `snake_case_with_trailing_underscore_` for class fields, and `snake_case` otherwise. Use
  numeric types from `"fpag/base/numeric.h"` (`i32`, `usize`, `f64`, etc.) instead of primitive
  C++ types.
- **Code style:** Use `#pragma once` for include guards and relative includes from project root.
  Prefer `std::string_view` over `std::string` and `std::span` over `std::vector` unless
  ownership retention is required.
- **One translation unit per header:** Every header under `src/` gets a sibling `.cc`, listed in
  its module's `BUILD.gn`. Put the header's out-of-line definitions in it; when it declares none,
  it holds only the license header and an `#include` of the header.

  The point is that the header becomes a translation unit of its own, which proves it is
  self-contained and lets `clang-tidy` and `clangd` analyse it directly. Without one, a missing
  include goes unseen: `clang-tidy` ran only on `.c`/`.cc`, and a header with no translation
  unit is compiled with a command that has no include paths, so the check reports nothing
  rather than failing. Generated translation units are the exception:
  `src/pipeline/embedded_std.h` and `src/pipeline/embedded_runtime.h` are implemented by
  generated files that already occupy the sibling name, so they are listed in `lint.py`
  until the generator is renamed.
- **Comments:** English only. Write comments sparingly-only for design rationale, invariants or
  safety explanations, non-obvious code, or `TODO`s. Do not restate code that is already clear.
- **Tooling is authoritative:** `.clang-format`, `.clang-tidy`, `CPPLINT.cfg`, and `typos.toml`
  enforce repository style. Use `format.py` and `lint.py --fix` to fix most issues automatically.
  New source files must carry the license header.
- **Module naming:** Module names stay abbreviated (`pkg`, `diag`, `cfg`). Directory, GN module,
  and namespace names must always match; full forms live in `docs/architecture.md`.
- **Wording:** The private LLVM fork is `llvm-alcy-fork` on first mention per document,
  and `the fork` thereafter.
- **Architecture decisions:** One decision, one record. Significant technical decisions get an ADR
  in `docs/adr/` (copy `docs/adr/0000-template.md`). Small, obvious changes do not need one.

## Changes and review

Keep changes focused; avoid mixing refactors with behavior changes unless they are inseparable.

When changing architecture, module boundaries, invariants, ownership/lifetime rules, or other
design-level contracts, update `docs/architecture.md` and add or update an ADR when appropriate.

When changing language behavior, update `docs/spec/` in the same change; spec and implementation
must not drift apart.

When changing behavior, tests must cover the new behavior and preserve relevant
invariants. The rule below is what "cover" means here; the full rationale is in
`docs/adr/0017-verification-strategy.md`.

## Testing rules

Every rule names an oracle. A test that only re-runs the code it tests cannot fail,
so it is not a test.

1. **A new error path needs a case that reaches it and asserts the diagnostic.** Not
   "the call returns" - assert the code, and the span, if the diagnostic names a
   location. A case that only checks the call survived belongs to the hostile-input
   layer, not to a unit test.
2. **A wrong answer needs a relation, not an example.** An example pins one input; a
   relation holds for every input or names a counterexample. Prefer a round trip
   through an independent implementation (`mangle` then `demangle`), then a fixed
   point, then a self-consistency of the output. If none of those exist, the
   property is not ready to be written yet.
3. **Recursion needs a budget, and the budget is shared.** A grammar that nests
   without a limit is a denial of service on a generated file. The limit is
   `base::MAX_NESTING` for every pass, because it is a property of the language
   rather than of one pass.
4. **A test with text does not need a file.** Use `pipeline::check_source` or
   `tests::add_sources` rather than a scratch directory. Reserve a real directory
   for a real reason: testing the filesystem, or writing an output artifact.
5. **A borrowed view outlives its owner or it is a defect.** Any type returned to a
   caller that hands out views must also return the storage those views borrow.
   AddressSanitizer catches this in debug; do not suppress it.
6. **A failure found by a fuzzer is checked in as a seed and a unit test.** The
   artifact alone is not a regression test, because it is only replayed by a fuzzer.
7. **Coverage may not go down.** `./build/scripts/check_coverage.py` fails when line
   coverage of `src/` drops below the recorded baseline. Move the baseline with
   `--update` only in a commit that says so.
8. **`fmt`, `lint`, and the unit suite are clean before you push.** Coverage and the
   fuzzer are not: the first is slow and the second needs clang's fuzzer runtime.

Which tool to reach for, in order: a unit test with an explicit expected value, a
property test, a sanitized hostile-input case, and libFuzzer for coverage-guided
exploration. Re-run everything with `./build/scripts/check.sh`.

The project is pre-MVP, so APIs and the IR are still allowed to change, but please call out
breaking changes in the commit message.
