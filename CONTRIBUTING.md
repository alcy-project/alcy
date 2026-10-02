# Contributing

## Prerequisites

- A C++20 toolchain: Clang, LLD, and libc++ (see [compiler/docs/build.md](compiler/docs/build.md)).
- GN and Ninja.
- Python via `uv` (`uv sync` sets up the environment; `uv run` prefixes commands).
- Alternatively, Nix provides the whole toolchain: `nix develop`.

## Workflow

Build, test, and check from the repository root:

```bash
typos
uv run ./tools/build.py --target=all --mode=debug
uv run ./tools/run.py --target=tests --mode=debug
uv run ./tools/check_e2e.py
uv run ./tools/check_exe.py
uv run ./tools/lint.py
uv run ./tools/format.py --dry-run
uv run ./tools/verify_static_linkage.py

# Or run all of the above commands at once:
./tools/check.sh

# Faster iteration (skips gn gen, gn check, and compdb; never for CI):
uv run ./tools/build.py --target=tests --fast
```

End-to-end acceptance cases live in `e2e/cases/<name>/` (sources plus
`expect.toml` with the expected exit code and output). The `check_e2e.py`
runner executes the built `alcy` binary against every case; add a case
when a user-visible behavior needs a regression anchor that does not
belong in unit tests.

To automatically fix code style and lint issues:

```bash
# Apply code formatting
uv run ./tools/format.py

# Fix lint issues (clang-tidy, clang-include-cleaner, etc.)
uv run ./tools/lint.py --fix

# Also apply fixes that may require manual verification
uv run ./tools/lint.py --fix-errors

```

Please make sure the CI pass before requesting a review.

```bash
# Locally (requires emcc and node on PATH):
./tools/check.sh --wasm

# Or directly:
uv run ./tools/run.py --target=tests --mode=debug \
  --build-subdir=build_wasm --target-os=emscripten
```

Notes:

- Keep different target OSes outputs in separate `--build-subdir` directories;
  reusing one output directory across target OSes leaves stale artifacts.

## Conventions

- **Architecture:** Follow [compiler/docs/architecture.md](compiler/docs/architecture.md) for module responsibilities,
  dependency direction, ownership/lifetime boundaries, allocation contracts, and core design
  principles. IR construction rules (type currency, operand factories, `SeqBuilder`, opcode
  conventions) live in [compiler/docs/ir.md](compiler/docs/ir.md).
- **Standard:** C++20. Follow the Google C++ Style Guide where it does not conflict with
  repository-specific conventions.
- **Explicitness:** Keep significant behavior visible at the call site. Do not use operator
  overloads for domain-specific or non-trivial behavior; use named functions instead. Avoid
  implicit conversions that obscure control flow, ownership, or cost. Avoid hardcoded values
  (unnamed numbers or strings) in domain logic; bind them to meaningful constants.
- **Ownership:** Make ownership and lifetime explicit in APIs. Prefer non-owning views for
  non-owning relationships and owning types only where ownership is part of the contract.
- **Dependencies:** Respect the dependency direction defined by `compiler/docs/architecture.md`. Do not
  introduce dependencies on higher-level modules for convenience, and do not create cyclic
  module dependencies.
- **Naming & Types:** `PascalCase` for classes/structs/enums, `UPPER_SNAKE_CASE` for constants,
  `snake_case_with_trailing_underscore_` for class fields, and `snake_case` otherwise. Use
  numeric types from `"fpag/base/numeric.h"` (`i32`, `usize`, `f64`, etc.) instead of primitive
  C++ types.
- **Noun-form names:** A type or module name names a thing, so its last word is a noun.
  Adjectives and participles are fine in front of it (`CheckedModule`, `VerifiedStorage`,
  `LoweredPackage`); a bare verb is not a name. Write the agent (`Parser`, `Lowerer`,
  `Desugarer`), the artifact (`FmtTemplate`, `VerificationError`), or the domain noun
  (`borrow`, because a borrow is a thing in the language). A module named after its action
  is the same mistake one level up, which is why the directory, the GN target, and the
  namespace are all `lowering` and not `lower`. `debug` is the one module that keeps a
  verb's name, as the convention every C++ toolchain already uses for assertions and
  logging.
- **Code style:** Use `#pragma once` for include guards and relative includes from project root.
  Prefer `std::string_view` over `std::string` and `std::span` over `std::vector` unless
  ownership retention is required.
- **One translation unit per header:** Every header under `compiler/` gets a sibling `.cc`, listed in
  its module's `BUILD.gn`. Put the header's out-of-line definitions in it; when it declares none,
  it holds only the license header and an `#include` of the header.

  The point is that the header becomes a translation unit of its own, which proves it is
  self-contained and lets `clang-tidy` and `clangd` analyse it directly. Without one, a missing
  include goes unseen: `clang-tidy` ran only on `.c`/`.cc`, and a header with no translation
  unit is compiled with a command that has no include paths, so the check reports nothing
  rather than failing. Generated translation units are the exception:
  `compiler/pipeline/embedded_std.h` is implemented by a generated file that already occupies
  the sibling name, so it is listed in `lint.py`
  until the generator is renamed.
- **Comments:** English only. Write comments sparingly-only for design rationale, invariants or
  safety explanations, non-obvious code, or `TODO`s. Do not restate code that is already clear.
- **Tooling is authoritative:** `.clang-format`, `.clang-tidy`, `CPPLINT.cfg`, and `typos.toml`
  enforce repository style. Use `format.py` and `lint.py --fix` to fix most issues automatically.
  New source files must carry the license header.
- **Module naming:** Module names stay abbreviated (`pkg`, `diag`). Directory, GN module,
  and namespace names must always match; full forms live in `compiler/docs/architecture.md`.
- **Wording:** The private LLVM fork is `llvm-alcy-fork` on first mention per document,
  and `the fork` thereafter.
- **Architecture decisions:** One decision, one record. Significant technical decisions get an ADR
  in `docs/adr/` (copy `docs/adr/0000-template.md`). Small, obvious changes do not need one. A
  reference names the file (`docs/adr/0011-symbol-mangling.md`), never the number alone: a number
  breaks when the log is renumbered, and a filename is what a reader can open.
- **ASCII Character Set**: All C++ source files (`compiler/`) must remain pure ASCII. Do not use
  non-ASCII Unicode characters in code or comments (e.g., em-dashes `—`, smart quotes `“”`, or arrows `→`).
  Use standard ASCII equivalents (`-`, `"..."`, `->`). Non-ASCII characters are permitted only in
  documentation (`docs/`).
- **No AI-Generated Walls of Text**: Respect maintainer review time. AI-generated walls of text,
  verbose speculative explanations, or conversational chat logs in issue descriptions or PRs are
  not accepted. Keep issue and PR descriptions brief, technical, and written in your own words.
  Focus strictly on: **What was broken? What was changed? How was it verified?**

## Changes and review

Keep changes focused; avoid mixing refactors with behavior changes unless they are inseparable.

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/): a type,
optionally a scope, then a subject, then a body.

```
<type>(<scope>): <subject>

<body>
```

The type says what kind of change this is, so a reader can filter the log before reading any of
it. The standard types are `feat` for a new capability, `fix` for a correction, `refactor` for a
change that neither adds nor repairs behaviour, `perf`, `test`, `docs`, `build`, `ci`, `chore`,
and `style`. Two the convention does not name are in use here: `check` for a change to one of
the acceptance scripts under `tools/`, and `borrow` for a change to the borrow checker, which is
large enough that its commits are worth reading as a series.

The scope is the module or area the change belongs to, and it is encouraged: `fix(borrow)`,
`feat(cli)`, `refactor(diag)`, `build(deps)`, `ci(wasm)`. It names where a reader would look
first, which a subject line cannot do in ten words. Use the directory or namespace name, and
prefer a scope that is already a `BUILD.gn` module over one invented for a single change. Omit
it only when a change genuinely spans the tree and any one scope would mislead.

The subject is imperative and lowercase, with no trailing period: what the change does, not
what was done. `fix: two at_mut results is a receiver conflict`, not `Fixed the at_mut issue`.

A `feat` or `fix` may carry a `BREAKING CHANGE:` footer, one per blank line after the body, for
a change that invalidates previous behaviour. The tree is young enough that none has been needed
yet, which is a reason to be sure before spending the one.

The body is optional and says why. A commit that needs more than a handful of lines has a message
that is too long: put the reasoning in a comment, a test, or the ADR rather than in the log. A
message that has to be read carefully to find the point is a message that is too long.

Record no abandoned attempts. Work that left nothing in the tree has nothing to point at later, so
the log becomes its only trace; if a mistake is worth remembering, it is worth a comment where the
next person meets the same wall.

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
7. **Coverage may not go down.** `./tools/check_coverage.py` fails when line
   coverage of `compiler/` drops below the recorded baseline. Move the baseline with
   `--update` only in a commit that says so.
8. **`fmt`, `lint`, and the unit suite are clean before you push.** So is the
   coverage ratchet: it runs in CI, and CI is not a substitute for running
   the thing yourself. The fuzzer is the exception - it needs clang's fuzzer
   runtime and its value decays as its corpus saturates, so it is a tool you
   reach for rather than a gate you clear.

Which tool to reach for, in order: a unit test with an explicit expected value, a
property test, a sanitized hostile-input case, and libFuzzer for coverage-guided
exploration. Re-run everything with `./tools/check.sh`.

The project is pre-MVP, so APIs and the IR are still allowed to change, but please call out
breaking changes in the commit message.
