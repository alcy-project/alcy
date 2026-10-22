# linter

A static analyzer for alcy, invoked as `alcy lint`. It reads a package
through the compiler frontend (parse, resolve, type-check) and reports
findings that do not stop the compiler: an unused binding, a loop over
`true`, a comparison of a value with itself. The compiler it runs beside
is the authority; the linter adds opinion, never semantics.

The module is a library, not a compiler stage. It depends on the
compiler; the compiler never depends on it. The cli calls
`linter::lint_package` / `linter::lint_single_file`, and the tests call
`linter::lint_source` to lint text without a file.

## Configuration

`.alcy/lint.toml` beside the goal package manifest holds the settings;
an absent file means every rule at its default. Only the goal package
is configured, and only its own modules are linted: staged standard
library sources and path dependencies are skipped.

```toml
# The level a rule not named below uses. One of allow, warn, deny.
level = "warn"

[rules]
# A string is the level; a table adds that rule's options.
unused-variable = "deny"
missing-docs = "allow"
too-many-arguments = { level = "warn", max-args = 5 }
```

An unknown rule name or level is an error: both are typos. Unknown keys
are ignored, like the other `.alcy/` files (ADR-0019). A `deny` finding
is an error, so `alcy lint` exits non-zero; a `warn` finding does not
change the exit code.

## Rules

| Id | Default | Reports |
| --- | --- | --- |
| `unused-variable` | warn | a binding no expression reads |
| `unused-mut` | warn | a `mut` binding that is never written |
| `shadow` | allow | a binding that shadows an enclosing one |
| `unreachable-code` | allow | a statement after control leaves the block |
| `while-true` | warn | `while true`, which a `loop` states better |
| `empty-loop` | warn | a `loop` with an empty body |
| `needless-return` | warn | a `ret` that ends a function body |
| `double-negation` | warn | an operator applied to itself |
| `self-comparison` | warn | a comparison whose operands are one expression |
| `nested-if` | warn | an `if` whose only statement is another `if` |
| `missing-docs` | allow | a `pub` item without a doc comment |
| `too-many-arguments` | warn | a function above `max-args` parameters |

`unreachable-code` is off by default because lowering already warns
about the same expression (`WD003`); it is there for a caller that
lints without lowering. A rule that suppresses a false positive is
preferred to one that guesses: `unused-mut` counts a method receiver as
a write, because the syntax does not say whether the receiver is taken
newly exclusively.

## Adding a rule

1. Declare the entry point in `rules/rules.h` and implement it under
   `rules/`. A rule reads `LintContext` - the checked package, the AST,
   the sources, and the resolved setting - and reports through
   `ctx.report<Key>(span, args...)`, which composes the catalog message
   and applies the configured level.
2. Add the message to `src/i18n/messages.def` under `/* linter */`.
3. Add a `LintCode` in `diag_code.h` and a row to the table in
   `rule.cc`. The table order is the configuration table order, so
   append rather than insert.
4. Add a case to `src/tests/linter/linter_test.cc` that reaches
   the finding and asserts the name and the span.

## Layout

- `lint.h` / `lint.cc`: the entry points and the file/config loading.
- `engine.h` / `engine.cc`: runs every enabled rule over the goal
  package modules and counts what it emitted.
- `rule.h` / `rule.cc`: the rule table.
- `config.h` / `config.cc`: `.alcy/lint.toml`.
- `context.h`: what one rule run reads and how it reports.
- `walk.h` / `walk.cc`: one traversal with callbacks, for rules that
  only inspect nodes.
- `bindings.h` / `bindings.cc`: the scope-resolving pass that says how
  often a name is read and written; `unused-variable`, `unused-mut`,
  and `shadow` read it.
- `rules/`: the rule implementations and their shared declarations.
