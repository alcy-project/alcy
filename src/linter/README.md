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

## Fixing

`alcy lint --fix` applies the repairs the rules are sure of and then
reports what remains. A pass lints, writes each file's non-overlapping
repairs, and lints again; a repair that overlaps one already applied is
left for a later pass, and the run stops after four. The final pass runs
on the rewritten files, so the findings it prints describe the bytes on
disk, and a denied finding a repair removed does not fail the run. A
file that cannot be written is reported and ends the pass loop.

## Rules

| Id | Default | Reports |
| --- | --- | --- |
| `unused-variable` | warn | a binding no expression reads |
| `unused-mut` | warn | a `mut` binding that is never written |
| `shadow` | allow | a binding that shadows an enclosing one |
| `unreachable-code` | allow | a statement after control leaves the block |
| `while-true` | warn | `while true`, which a `loop` states better |
| `empty-loop` | warn | a `loop` with an empty body |
| `double-negation` | warn | an operator applied to itself |
| `self-comparison` | warn | a comparison whose operands are one expression |
| `nested-if` | warn | an `if` whose only statement is another `if` |
| `useless-cast` | warn | a cast to the type the value already has |
| `float-equality` | allow | an equality test on floating-point values |
| `needless-borrow` | warn | a borrow of a dereference that is the reference |
| `lossy-cast` | allow | a cast that narrows the value's scalar type |
| `missing-docs` | allow | a `pub` item without a doc comment |
| `too-many-arguments` | warn | a function above `max-args` parameters |

`unreachable-code` is off by default because lowering already warns
about the same expression (`WD003`); it is there for a caller that
lints without lowering. `float-equality` is opt-in because comparing
floats exactly is sometimes the point; the rule reports the comparison
that is usually a mistake, not every one. A rule that suppresses a
false positive is preferred to one that guesses: `unused-mut` counts a
method receiver as a write, because the syntax does not say whether the
receiver is taken newly exclusively.

The type-based rules read the analyzer's recorded types, not the
source spelling. `types.h` answers what type an expression was checked
at and whether that type is a float or a shared reference; a cast whose
two types are the same, an equality over a float, `&*r` over a shared
reference, and a cast from a wider integer or float are the findings. `useless-cast` and `needless-borrow`
also offer a machine-applicable repair (`ctx.fix`) when the expression
they would put in place binds tightly enough that the regrouping cannot
change the meaning.

## Adding a rule

1. Define the rule as a type in `rules/rules.h` - the id lint.toml
   addresses, the default level, the code, the summary, and a static
   `run` - and implement `run` under `rules/`. A rule reads
   `LintContext` - the checked package, the AST, the sources, and the
   resolved setting - and reports through
   `ctx.report<Key>(span, args...)`, which composes the catalog message
   and applies the configured level. `ctx.fix(finding, span, bytes)`
   attaches the repair a client may apply without asking, and
   `ctx.label` attaches a note. The `RuleType` concept holds the type
   to the shape before anything compiles.
2. Add the message to `src/i18n/messages.def` under `/* linter */`.
3. Add a `LintCode` in `diag_code.h` and the type to the list in
   `rule.cc`. The list's order is the configuration table order, so
   append rather than insert; the registry folds the list into the
   table the runtime reads.
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
- `types.h` / `types.cc`: the checked-type query the type-based rules
  read, including the conservative answer for a generic body.
- `fix.h` / `fix.cc`: collecting a pass's repairs and applying one
  file's edits, which `--fix` drives.
- `rules/`: the rule implementations and their shared declarations.
