# ADR-0082: The linter is a library over the frontend

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-19

## Context

`alcy lint` should read a package the way clippy and clang-tidy read a
program: after the frontend has resolved and type-checked it, so a
finding is about a program that compiles, and configurable per project
through `.alcy/lint.toml` (ADR-0019). The analysis it needs - the typed
AST, the module tree, the checked package - is already built by the
frontend. Rebuilding any of it inside a second tool would make the two
read source differently, which is the failure mode every language tool
that grew its own parser has had.

## Decision

- `src/linter/` is a module of the toolchain tree, not a stage of the
  compiler. It depends on the compiler (`//src:alcy_lib`), the cli
  depends on it, and no compiler target depends on it - the
  dependency direction is the boundary, and gn check holds it. The
  include spelling is `linter/...` and the namespace is `linter`.
- One entry point runs the analysis the compiler runs, without
  lowering, over a resolved tree (`pipeline::analyze_tree`). Rules read
  the AST and the checked package, then the linter finishes the
  frontend (lower and borrow-check) so a program the compiler would
  refuse is not reported lint-clean.
- A finding is a `diag::Diagnostic` with `Stage::Lint`, the rule name in
  `check_name`, and the rule code beside it. A rule configures to
  `allow`, `warn`, or `deny`; `deny` composes an error the run fails on.
  Rendering, `--json`, colour, and the exit code are the compiler
  diag/cli machinery, so lint output and compiler output cannot drift.
- `.alcy/lint.toml` holds a top-level `level` and a `[rules]` table. A
  rule is named by its id (`unused-variable`); an unknown id or an
  unknown level is an error, because both are typos a reader wants to
  hear about. Only the goal package's `.alcy/` is read, and only the
  goal package modules are linted: staged standard-library sources and
  path dependencies are skipped.
- A rule is a type: the id, default level, code, and summary as
  static members, and the check as a static `run`, defined in
  `linter/rules/`. A concept holds every rule to that shape, and
  the registry folds the type list into the runtime table the
  engine iterates and lint.toml resolves against - the dispatch is
  one function pointer per rule, generated rather than
  hand-maintained. There is no virtual dispatch, matching the
  compiler no-runtime-polymorphism rule. The type list's order is
  the index the parsed configuration is aligned with, so a rule is
  appended and never inserted.
- `alcy lint --fix` writes the repairs a rule attached and lints again,
  up to four passes. A repair is the rule's assertion that it knows the
  bytes it replaces; the client applies it without asking. An edit that
  overlaps one already applied is skipped, a file that cannot be written
  is reported, and the final pass reports the bytes on disk, so a denied
  finding the repair removed does not fail the run.

## Consequences

The linter gets the frontend, the diagnostic renderer, the source
snippet, and the JSON envelope for free. The frontend grew one seam,
`analyze_tree`, which build and check now also compose; a new rule is
a type under `linter/rules/` and one line in the registry's list.

`missing-docs` and `shadow` default to `allow`: they are style choices
a project opts into. `unreachable-code` is also opt-in, because the
lowering pass already warns about the same expression (WD003) and the
lint exists for a caller that lints without lowering.

Out of scope for this record: per-rule option schemas beyond the one
`max-args` field, plugin or dynamically loaded rules, and linting
dependencies.
