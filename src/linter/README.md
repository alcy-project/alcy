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
# Run lowering and the borrow check after the rules (default true).
# borrow-checks = true

# The level a rule not named below uses. One of allow, warn, deny.
level = "warn"

[rules]
# A string is the level; a table adds that rule's options.
unused-variable = { level = "deny", ignore = ["i", "j"] }
missing-docs = "allow"
too-many-arguments = { level = "warn", max-args = 5 }
```

An unknown rule name or level is an error: both are typos. Unknown keys
are ignored, like the other `.alcy/` files (ADR-0019). `ignore` lists
names the rule must not report; the binding rules and `unused-import`
read it. A `deny` finding is an error, so `alcy lint` exits non-zero;
a `warn` finding does not change the exit code.

## Fixing

`alcy lint --fix` applies the repairs the rules are sure of and then
reports what remains. A pass lints, writes each file's non-overlapping
repairs, and lints again; a repair that overlaps one already applied is
left for a later pass, and the run stops after four. The final pass runs
on the rewritten files, so the findings it prints describe the bytes on
disk, and a denied finding a repair removed does not fail the run. A
file that cannot be written is reported and ends the pass loop.

## Performance

A run is the frontend, the rules, and, unless borrow-checks is false,
the lowering and borrow tail. The rules on a module share one walk, one
binding table, and the name-usage table the walk records; the binding
table is built only when an enabled rule reads it, and so is the usage
table. The tail is most of the cost and the rules do not need it, so
borrow-checks = false stops after analysis for a faster edit loop. The
run then does not report a borrow error, and alcy check remains the
complete gate.

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
| `bool-comparison` | warn | a bool compared with a true or false literal |
| `dead-code` | allow | a function no call in the package reaches |
| `missing-docs` | allow | a `pub` item without a doc comment |
| `too-many-arguments` | warn | a function above `max-args` parameters |
| `unused-import` | warn | an import no path in the module names |

`unreachable-code` and `dead-code` are off by default: the lowering
pass already warns about the same expression (`WD003`), and a package
in progress always has functions waiting for their first caller.
`float-equality` is opt-in because comparing floats exactly is
sometimes the point; the rule reports the comparison that is usually a
mistake, not every one. A rule that suppresses a
false positive is preferred to one that guesses: `unused-mut` counts a
method receiver as a write, because the syntax does not say whether the
receiver is taken newly exclusively.

`unused-field` reads the field names the walk records. A field is read
when a projection or a destructuring pattern names it; the
constructor's initializer is a write and does not count, which is the
question the rule answers. A `pub` field is the package's surface and
is never reported; the struct's own `pub` does not make every field
one.

`dead-code` reads the checker's call graph, not the source: a finding
is a private function whose (module, signature) pair no recorded call
target names. The entry point, `pub` declarations, and spec
implementations are the surface the rule keeps out of the question; the
goal package alone is read, so the toolchain's own functions are never
findings. It is opt-in because a package in progress always has
functions waiting for their first caller.

`unused-import` reads the path heads the walk recorded. An import is
used when the module names its local name as the head of a value path, a
type path, a pattern path, or a struct literal; a `pub use` is a
re-export and an underscore alias imports for resolution's sake, so
neither is reported. The rule removes the import and its line with a
repair.

The type-based rules read the analyzer's recorded types, not the
source spelling. `types.h` answers what type an expression was checked
at and whether that type is a float or a shared reference; a cast whose
two types are the same, an equality over a float, `&*r` over a shared
reference, a cast from a wider integer or float, and a bool compared
with a true or false literal are the findings. `useless-cast`,
`needless-borrow`, and `bool-comparison` also offer a machine-applicable
repair (`ctx.fix`) when the expression they would put in place binds
tightly enough that the regrouping cannot change the meaning.

## Adding a rule

1. Define the rule as a type in `rules/rules.h` - the id lint.toml
   addresses, the default level, the code, and the summary - and one
   check shape: a whole-module `run`, or a `check_expr` / `check_item`
   / `check_block` hook the engine's shared walk dispatches to.
   Implement it under `rules/`. A module rule that needs the binding or
   usage table declares `READS_BINDINGS` or `READS_USAGE`; the engine
   builds it once per module. A rule reads
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
- `engine.h` / `engine.cc`: builds the active rule lists, the
  per-module binding table, and the usage table; dispatches the one
  walk; and counts what the rules emitted.
- `rule.h` / `rule.cc`: the rule table.
- `config.h` / `config.cc`: `.alcy/lint.toml`.
- `context.h`: what one rule run reads and how it reports.
- `walk.h` / `walk.cc`: one traversal with callbacks, for rules that
  only inspect nodes.
- `bindings.h` / `bindings.cc`: the scope-resolving pass that builds
  one binding table per module; `unused-variable`, `unused-mut`, and
  `shadow` read it.
- `types.h` / `types.cc`: the checked-type query the type-based rules
  read, including the conservative answer for a generic body.
- `usage.h` / `usage.cc`: the path heads the walk records for
  `unused-import`.
- `fix.h` / `fix.cc`: collecting a pass's repairs and applying one
  file's edits, which `--fix` drives.
- `rules/`: the rule implementations and their shared declarations.
