# Contributing

## Prerequisites

- A C++20 toolchain: Clang, LLD, and libc++ (see
  [src/docs/build.md](src/docs/build.md)).
- GN and Ninja.
- Python via `uv` (`uv sync` sets up the environment; `uv run` prefixes
  commands).
- The tree-sitter CLI and Bun, for the language's grammar (see
  [treesitter/README.md](treesitter/README.md)).
- Alternatively, Nix provides the whole toolchain: `nix develop`. The wasm
  checks need Emscripten too, which the default shell leaves out to keep the
  build matrix light; they run from the shell that carries it:
  `nix develop .#wasm --command uv run ./tools/check.py --nix`.

## Workflow

Build, test, and check from the repository root. Every tool takes
`--help`; the invocations below are the ones the workflow needs, not the
full option set:

```bash
typos
uv run ./tools/build.py --target=all --mode=debug
uv run ./tools/run.py --target=tests --mode=debug
uv run ./tools/check_cases.py --steps=cli
uv run ./tools/check_cases.py --steps=program
# The standard library states its own behaviour in `#[test] fn`s, and this
# runs them: one `alcy test` per member package, so a member that declares
# no item yet is the only one excused.
uv run ./tools/check_stdlib_tests.py
# The compile-time evaluator over the acceptance corpus: every
# case that compiles is also evaluated function by function:
uv run ./tools/check_comp.py
# The .al sources are the formatter's own check: laid out as alcy fmt
# writes them, idempotent under a second pass, unchanged in verdict.
uv run ./tools/check_fmt.py
# lint.py also runs the format dry run, the license headers, and the
# tree's TOML, Markdown, rule, and Python checks.
uv run ./tools/lint.py
uv run ./tools/format.py --dry-run
uv run ./tools/verify_static_linkage.py --build-dir=out/build
# Every target operating system is configured, not built: a toolchain
# definition that cannot produce one fails here.
uv run ./tools/cross.py --gen-only
# The guide's generator has its own tests and its own no-LLVM build:
uv run ./tools/site.py test

# The grammar needs no toolchain but the tree-sitter CLI and Bun:
uv run ./tools/check_treesitter.py --grammar

# Or run every local check at once. CI asks each of these as its own job,
# and check.py asks the same checks plus the ones below:
uv run ./tools/check.py

# Faster iteration (skips gn gen, gn check, and compdb; never for CI):
uv run ./tools/build.py --target=tests --fast

# Lint only the files this branch changed, against the merge base with
# main. CI still runs the full tree:
uv run ./tools/lint.py --changed

# Where the last build's time went (a report, not a check):
uv run ./tools/build_time.py

# Debug builds carry line tables; --debug-info=full restores -g3 detail:
uv run ./tools/build.py --target=tests --debug-info=full

# A debug build carries AddressSanitizer. --sanitizer=thread swaps in
# ThreadSanitizer, which is what asks whether two workers touch one object:
uv run ./tools/build.py --target=tests --sanitizer=thread --build-subdir=tsan
```

Acceptance cases live in `e2e/cases/<name>/` and `e2e/wasm/<name>/`.
Each directory holds its sources and a `case.toml`: `[cli]` says how the
compiler is called and what it should answer, and `[program]` says what
the program it builds should answer. A case may carry either or both.
`check_cases.py` runs the steps a caller selects; `--mode=sanitized`,
`--mode=wasm`, and `--mode=qemu` ask the same program step of a different
lowering or target. `check.py` runs the qemu mode too; pass `--no-qemu`
when clang or qemu-user is unavailable. The
cases a wasm run cannot hold name the reason in
`[program.sweep.wasm] skip`, so a refusal is a decision beside the case
rather than a list elsewhere.

The grammar in `treesitter/` is a second reading of the language, written
against `docs/spec/` rather than against the parser. `grammar.js` follows
`docs/spec/grammar.ebnf` production for production, so a change to the
grammar means changing the grammar there too. `tools/check_treesitter.py
--grammar` regenerates the parser and fails when the committed sources do
not match, runs the corpus, and checks the queries; adding
`--differential` compares every `.al` file against the compiler's own
verdict, which is the half of the check that can notice the specification,
the parser, and the grammar disagreeing. `check.py` runs both and
`measure_checks.py` times both. See `treesitter/README.md`.

Performance work has its own loop: a generated corpus, instruction counts
instead of wall clock, a profile that names the function, and an emitted-IR
comparison as the equivalence proof. `uv run ./tools/perf.py` runs the
measurements, and
[src/docs/performance.md](src/docs/performance.md) is the loop they
are steps in.

To fix code style and lint issues:

```bash
# Everything the tools can fix: clang-format and license headers over
# the sources, gn format, ast-grep's rule fixes, rumdl, and ruff --
# plus taplo when it is on PATH.
uv run ./tools/format.py

# Lint, with clang-tidy's fixes applied first:
uv run ./tools/lint.py --fix

# The same, allowing fixes that may need a manual look:
uv run ./tools/lint.py --fix-errors
```

`uv run ./tools/format.py --dry-run` reports the same checks without
writing, and `uv run ./tools/lint.py` runs them plus clang-tidy. `taplo`
is a system tool rather than a uv dependency: the scripts use it when it
is installed, and CI's tree checks enforce it regardless.

Please make sure the CI pass before requesting a review.

`uv run ./tools/check.py` runs every local check: the same commands as above,
against the debug build and again against the release one (the unit
tests, the e2e cases, and the exe programs -- a bug that only a release
build has is invisible to the debug checks), plus the benchmark smoke, the
WebAssembly tests, the direct-backend wasm acceptance, the exe programs
against that backend, and the playground smoke, which need `emcc` and
`bun` on `PATH`. `uv run ./tools/check.py --help`
lists what each flag skips.

```bash
# Everything (requires emcc and bun on PATH):
uv run ./tools/check.py

# Skip the wasm checks:
uv run ./tools/check.py --no-wasm

# Skip the release checks:
uv run ./tools/check.py --no-release

# Just the wasm tests:
uv run ./tools/run.py --target=tests --mode=debug \
  --build-subdir=build_wasm --target-os=emscripten
```

Every check prints its own duration, and the run ends with the phases
sorted by duration, slowest first:

```text
==> timings (slowest first)
        812.3s  uv run ./tools/check_coverage.py
         95.4s  uv run ./tools/check_cases.py --steps=program --build-subdir=build_release
        ...
==> total 1832.1s
```

For an iteration pass that skips the heavyweight ones — the coverage
ratchet, the sanitized exe cases, the WebAssembly tests — say so instead
of waiting:

```bash
uv run ./tools/check.py --no-coverage --no-sanitize --no-wasm
```

`--help` lists every skip. CI still runs each check as its own job, so the
full run remains the bar before a review.

Notes:

- Keep different target OSes outputs in separate `--build-subdir` directories;
  reusing one output directory across target OSes leaves stale artifacts.

## Conventions

- **Architecture:** Follow [src/docs/architecture.md](src/docs/architecture.md)
  for module responsibilities, dependency direction, ownership/lifetime
  boundaries, allocation contracts, and core design principles. IR construction
  rules (type currency, operand factories, `SeqBuilder`, opcode conventions)
  live in [src/docs/ir.md](src/docs/ir.md).

- **Standard:** C++20. Follow the Google C++ Style Guide where it does not
  conflict with repository-specific conventions.

- **Explicitness:** Keep significant behavior visible at the call site. Do not
  use operator overloads for domain-specific or non-trivial behavior; use named
  functions instead. Avoid implicit conversions that obscure control flow,
  ownership, or cost. Avoid hardcoded values (unnamed numbers or strings) in
  domain logic; bind them to meaningful constants.

- **Ownership:** Make ownership and lifetime explicit in APIs. Prefer non-owning
  views for non-owning relationships and owning types only where ownership is
  part of the contract.

- **Dependencies:** Respect the dependency direction defined by
  `src/docs/architecture.md`. Do not introduce dependencies on higher-level
  modules for convenience, and do not create cyclic module dependencies.

- **Naming & Types:** `PascalCase` for classes/structs/enums, `UPPER_SNAKE_CASE`
  for constants, `snake_case_with_trailing_underscore_` for class fields, and
  `snake_case` otherwise. Use numeric types from `"fpag/base/numeric.h"` (`i32`,
  `usize`, `f64`, etc.) instead of primitive C++ types.

- **Noun-form names:** A type or module name names a thing, so its last word is
  a noun. Adjectives and participles are fine in front of it (`CheckedModule`,
  `VerifiedStorage`, `LoweredPackage`); a bare verb is not a name. Write the
  agent (`Parser`, `Lowerer`, `Desugarer`), the artifact (`FmtTemplate`,
  `VerificationError`), or the domain noun (`borrow`, because a borrow is a
  thing in the language). A module named after its action is the same mistake
  one level up, which is why the directory, the GN target, and the namespace are
  all `lowering` and not `lower`. `debug` is the one module that keeps a verb's
  name, as the convention every C++ toolchain already uses for assertions and
  logging.

- **Code style:** Use `#pragma once` for include guards and relative includes
  from project root. Prefer `std::string_view` over `std::string` and
  `std::span` over `std::vector` unless ownership retention is required.

- **One translation unit per header:** Every header under `src/` gets a sibling
  `.cc`, listed in its module's `BUILD.gn`. Put the header's out-of-line
  definitions in it; when it declares none, it holds only the license header and
  an `#include` of the header.

  The point is that the header becomes a translation unit of its own, which
  proves it is self-contained and lets `clang-tidy` and `clangd` analyse it
  directly. Without one, a missing include goes unseen: `clang-tidy` ran only on
  `.c`/`.cc`, and a header with no translation unit is compiled with a command
  that has no include paths, so the check reports nothing rather than failing.
  Generated translation units are the exception: `src/pipeline/embedded_std.h`
  is implemented by a generated file that already occupies the sibling name, so
  it is listed in `lint.py` until the generator is renamed.

- **Comments:** English only. Write comments sparingly-only for design
  rationale, invariants or safety explanations, non-obvious code, or `TODO`s. Do
  not restate code that is already clear.

- **Tooling is authoritative:** `.clang-format`, `.clang-tidy`, `CPPLINT.cfg`,
  `taplo.toml`, `.rumdl.toml`, `sgconfig.yml`, and `pyproject.toml` (ruff,
  jactionlint) enforce repository style. Use `format.py` and `lint.py --fix`,
  then the fixes listed above, to correct most issues automatically. The
  ast-grep rules under `tools/ast-grep/rules/` state the conventions the other
  linters cannot: fixed-width type aliases, owned memory, and the constructs the
  project does without. New source files must carry the license header.

- **Module naming:** Module names stay abbreviated (`pkg`, `diag`). Directory,
  GN module, and namespace names must always match; full forms live in
  `src/docs/architecture.md`.

- **Wording:** The private LLVM fork is `llvm-alcy-fork` on first mention per
  document, and `the fork` thereafter.

- **The specification is the record.** The language and the standard library are
  specified in `docs/spec/`; how the toolchain itself is put together is the
  record in `src/docs/architecture.md`. A change that alters observable behavior
  must update `docs/spec/` in the same change, with a conformance case where
  one applies. Small, obvious changes do not need a separate decision record.

- **ASCII Character Set**: All C++ source files (`src/`) must remain pure ASCII.
  Do not use non-ASCII Unicode characters in code or comments (e.g., em-dashes
  `—`, smart quotes `“”`, or arrows `→`). Use standard ASCII equivalents (`-`,
  `"..."`, `->`). Non-ASCII characters are permitted only in documentation
  (`docs/`).

- **No AI-Generated Walls of Text**: Respect maintainer review time.
  AI-generated walls of text, verbose speculative explanations, or
  conversational chat logs in issue descriptions or PRs are not accepted. Keep
  issue and PR descriptions brief, technical, and written in your own words.
  Focus strictly on: **What was broken? What was changed? How was it verified?**

## Changes and review

Keep changes focused; avoid mixing refactors with behavior changes unless they
are inseparable.

A commit is the smallest change to the tree that is complete on its own. Its
message must make sense to a reader who has none of the context that produced
it: no review thread, no issue, no agent session, and no CI log. Such a commit
can be reviewed, reverted, or cherry-picked without a note beside it.

The repository carries Git hooks in `.githooks/`. Enable them once per clone:

```bash
git config core.hooksPath .githooks
```

`pre-commit` runs the tree's formatting checks and lints the staged source
files, and `commit-msg` checks the message against the convention below. Lint
reads the working-tree copy of a staged file, so a partially staged edit is
checked as it sits on disk. A work in progress can bypass both with
`git commit --no-verify`; CI still asks every question, so a hook is
a way to fail earlier rather than a second gate to remember.

Commit messages follow
[Conventional Commits](https://www.conventionalcommits.org/): a type, optionally
a scope, then a subject, then a body.

```text
<type>(<scope>): <subject>

<body>
```

The type says what kind of change this is, so a reader can filter the log before
reading any of it:

- `feat`: a new capability
- `fix`: a correction to behaviour that already exists
- `refactor`: a change that neither adds nor repairs behaviour
- `perf`: faster or leaner without a behaviour change
- `test`: tests only
- `docs`: documentation only
- `build`: the build system or dependencies
- `ci`: CI configuration
- `chore`: maintenance that changes no product behaviour
- `style`: formatting only - whitespace, line breaks, and names that do not
  change what the program does

The scope is the module or area the change belongs to, and it is encouraged:
`fix(borrow)`, `feat(cli)`, `refactor(diag)`, `build(deps)`, `ci(wasm)`. It
names where a reader would look first, which a subject line cannot do in ten
words. Use the directory or namespace name, and prefer a scope that is already a
`BUILD.gn` module over one invented for a single change. Omit it only when a
change genuinely spans the tree and any one scope would mislead.

The subject is imperative and lowercase, with no trailing period: what the
change does, not what was done.
`fix: report two at_mut results as a receiver conflict`, not
`Fixed the at_mut issue`.

Conventional Commits sets no line widths, so this project borrows the
conventions around it: aim for 72 columns in the subject and in every
line of the body, and never exceed 100. The target is what `git log`
renders well; the cap is what the standard commitlint configuration
enforces, so a written rule and a future check cannot disagree.

A breaking change invalidates previous behaviour. Mark it with `!` after the
type or scope (`feat!: ...`, `feat(cli)!: ...`), with a `BREAKING CHANGE:`
footer one blank line after the body, or with both. The footer names what
callers must change. The tree is young enough that none has been needed yet,
which is a reason to be sure before spending the one.

The body is optional and says why. A commit that needs more than a handful of
lines has a message that is too long: put the reasoning in a comment, a test, or
the specification rather than in the log. A message that has to be read
carefully to find the point is a message that is too long.

Record no process. The log says what changed in the tree and why, not how the
change was reached. Review comments, tool runs, test counts, and requests are
recorded where they happened.

```text
BAD subject:
    fix: address review for codegen
    docs: apply agent suggestions

BAD footer:
    Verification: e2e 862 cases pass, 1631 unit tests pass

GOOD:
    fix(codegen): keep the borrow result of a dropped temporary

    The temporary was dropped before lowering read its borrow, so the
    receiver lost the call's result.
```

Record no abandoned attempts. Work that left nothing in the tree has nothing to
point at later, so the log becomes its only trace; if a mistake is worth
remembering, it is worth a comment where the next person meets the same wall.

When changing architecture, module boundaries, invariants, ownership/lifetime
rules, or other design-level contracts, update `src/docs/architecture.md`, which
is the record of how the compiler and the tools are put together, and keep it in
the same change.

When changing language behavior, update `docs/spec/` in the same change; spec
and implementation must not drift apart.

When changing behavior, tests must cover the new behavior and preserve relevant
invariants. The rule below is what "cover" means here.

## Testing rules

Every rule names an oracle. A test that only re-runs the code it tests cannot
fail, so it is not a test.

1. **A new error path needs a case that reaches it and asserts the diagnostic.**
   Not "the call returns" - assert the code, and the span, if the diagnostic
   names a location. A case that only checks the call survived belongs to the
   hostile-input layer, not to a unit test.
2. **A wrong answer needs a relation, not an example.** An example pins one
   input; a relation holds for every input or names a counterexample. Prefer a
   round trip through an independent implementation (`mangle` then `demangle`),
   then a fixed point, then a self-consistency of the output. If none of those
   exist, the property is not ready to be written yet.
3. **Recursion needs a budget, and the budget is shared.** A grammar that nests
   without a limit is a denial of service on a generated file. The limit is
   `base::MAX_NESTING` for every pass, because it is a property of the language
   rather than of one pass.
4. **A test with text does not need a file.** Use `pipeline::check_source` or
   `tests::add_sources` rather than a scratch directory. Reserve a real
   directory for a real reason: testing the filesystem, or writing an output
   artifact.
5. **A borrowed view outlives its owner or it is a defect.** Any type returned
   to a caller that hands out views must also return the storage those views
   borrow. AddressSanitizer catches this in debug; do not suppress it.
6. **A failure found by a fuzzer is checked in as a seed and a unit test.** The
   artifact alone is not a regression test, because it is only replayed by a
   fuzzer.
7. **Coverage may not go down.** `./tools/check_coverage.py` fails when line
   coverage of `src/` drops below the recorded baseline. Move the baseline with
   `--update` only in a commit that says so.
8. **`fmt`, `lint`, and the unit suite are clean before you push.** So is the
   coverage ratchet: it runs in CI, and CI is not a substitute for running
   the thing yourself. The fuzzer is the exception - it needs clang's fuzzer
   runtime and its value decays as its corpus saturates, so it is a tool you
   reach for rather than a check that has to pass.

Which tool to reach for, in order: a unit test with an explicit expected value,
a property test, a sanitized hostile-input case, and libFuzzer for
coverage-guided exploration. Re-run everything with `uv run ./tools/check.py`.

The project is pre-1.0, so APIs and the IR are still allowed to change, but
please call out breaking changes in the commit message.
